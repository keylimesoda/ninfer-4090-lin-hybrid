// ninfer-kld: score a .ninfer artifact against a lovelace-kld base file (sparse top-k reference
// distributions over recorded agent turns) and report the same metrics as `lovelace-kld compare`:
// KL(P_base || Q) over the base top-k ids plus one tail bucket, top-1 agreement, and the NLL of
// the actual next token, per window, per depth bucket, and overall. The JSON result has the
// schema of lovelace-kld compare, so lovelace's eval/kld/summarize.py reads it unchanged.
//
// Before scoring, every window is tokenized with the artifact tokenizer exactly as lovelace-kld
// does (context and reply tokenized separately, special tokens parsed) and must reproduce the
// base's token hash, scored-span start, and next-token ids; any mismatch aborts the run.
// Q is the engine's own causal-scoring distribution: BF16 output logits, normalized over the
// token domain, with the selected Main KV representation.

#include "kld_math.h"

#include "ninfer/engine.h"
#include "product/logging/logging.h"
#include "product/logging/startup_log.h"

#include <cuda_runtime.h>
#include <nlohmann/json.hpp>
#include <spdlog/logger.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using json  = nlohmann::ordered_json;
using Clock = std::chrono::steady_clock;

constexpr char BASE_MAGIC[8]    = {'L', 'V', 'K', 'L', 'D', 'B', 'A', 'S'};
constexpr uint32_t BASE_VERSION = 1;

struct Options {
    std::string artifact, windows, base, out;
    std::string kv_name = "rk4v4-e8";
    ninfer::KvCacheStorage kv = ninfer::KvCacheStorage::RK4V4E8;
    std::vector<std::string> buckets;
    uint32_t max_context = 0;  // 0 = longest selected window, rounded up to 256
    int device           = 0;
    ninfer::product::LogLevel log_level = ninfer::product::LogLevel::Info;
};

const char* USAGE =
    "usage: ninfer-kld <model.ninfer> --windows W.jsonl --base base.bin --out result.json\n"
    "       [--kv-dtype bf16|int8|fp8|rk8v4|rk4v4|rk4v4-e8|rk2v4-e8|nvfp4|k8v4] (default rk4v4-e8)\n"
    "       [--buckets B[,B...]] [--max-context N] [--device N]\n"
    "       [--log-level trace|debug|info|warning|error|critical|off]\n";

std::runtime_error fail(const std::string& msg) { return std::runtime_error(msg); }

ninfer::KvCacheStorage parse_kv(std::string_view s) {
    using K = ninfer::KvCacheStorage;
    if (s == "bf16") return K::BFloat16;
    if (s == "int8") return K::Int8Group64;
    if (s == "fp8") return K::Fp8E4M3Row256;
    if (s == "rk8v4") return K::RotatedInt8KeyInt4ValueGroup64;
    if (s == "rk4v4") return K::RotatedInt4KeyInt4ValueGroup64;
    if (s == "rk4v4-e8") return K::RK4V4E8;
    if (s == "rk2v4-e8") return K::RK2V4E8;
    if (s == "nvfp4") return K::Nvfp4Group16;
    if (s == "k8v4") return K::Fp8KeyNvfp4Value;
    throw fail("invalid --kv-dtype: " + std::string(s));
}

std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t end = std::min(s.find(',', start), s.size());
        if (end > start) out.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

Options parse_options(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]).starts_with("--")) throw fail("artifact path is required");
    Options o;
    o.artifact = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) throw fail("missing value for " + k);
            return argv[++i];
        };
        if (k == "--windows") o.windows = val();
        else if (k == "--base") o.base = val();
        else if (k == "--out") o.out = val();
        else if (k == "--kv-dtype") {
            o.kv_name = val();
            o.kv      = parse_kv(o.kv_name);
        } else if (k == "--buckets") o.buckets = split_commas(val());
        else if (k == "--max-context") o.max_context = uint32_t(std::stoul(val()));
        else if (k == "--device") o.device = std::stoi(val());
        else if (k == "--log-level") o.log_level = ninfer::product::parse_log_level(val());
        else throw fail("unknown option " + k);
    }
    if (o.windows.empty() || o.base.empty() || o.out.empty())
        throw fail("--windows, --base and --out are required");
    return o;
}

// ---------------------------------------------------------------------------------------------
// windows.jsonl and the base file (formats: lovelace eval/kld/README.md)

struct Window {
    std::string id, session, bucket, text;
    size_t start_byte = 0;  // UTF-8 byte offset of score_start_char
};

size_t utf8_offset(const std::string& s, int64_t cp) {
    int64_t seen = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (seen == cp) return i;
            ++seen;
        }
    }
    if (seen == cp) return s.size();
    throw fail("score_start_char beyond end of text");
}

std::map<std::string, Window> load_windows(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw fail("cannot open " + path);
    std::map<std::string, Window> out;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        const json j = json::parse(line);
        Window w;
        w.id         = j.at("id").get<std::string>();
        w.session    = j.value("session", "");
        w.bucket     = j.value("depth_bucket", "all");
        w.text       = j.at("text").get<std::string>();
        w.start_byte = utf8_offset(w.text, j.at("score_start_char").get<int64_t>());
        out[w.id]    = std::move(w);
    }
    if (out.empty()) throw fail("no windows in " + path);
    return out;
}

struct BasePos {
    int32_t next_id;
    float next_lp;
    double lse;
    std::vector<kld::TopEntry> top;
};

struct BaseWindow {
    std::string id;
    int32_t n_prefix = 0, n_scored = 0;
    uint64_t hash = 0;
    std::vector<BasePos> pos;
};

struct BaseFile {
    uint32_t topk = 0, n_vocab = 0;
    uint64_t model_hash = 0;
    json meta;
    std::vector<BaseWindow> windows;
};

BaseFile read_base(const std::string& path) {
    static_assert(sizeof(kld::TopEntry) == 8, "TopEntry must be packed (i32, f32)");
    std::ifstream f(path, std::ios::binary);
    if (!f) throw fail("cannot open " + path);
    auto get = [&](void* p, size_t n) {
        if (!f.read(static_cast<char*>(p), std::streamsize(n))) throw fail(path + ": truncated");
    };
    char magic[8];
    get(magic, 8);
    if (memcmp(magic, BASE_MAGIC, 8) != 0) throw fail(path + ": not a lovelace-kld base file");
    uint32_t version, n_windows, meta_len;
    BaseFile b;
    get(&version, 4);
    if (version != BASE_VERSION) throw fail(path + ": unsupported version " + std::to_string(version));
    get(&b.topk, 4);
    get(&n_windows, 4);
    get(&b.n_vocab, 4);
    get(&b.model_hash, 8);
    get(&meta_len, 4);
    std::string m(meta_len, '\0');
    get(m.data(), meta_len);
    b.meta = json::parse(m);
    b.windows.resize(n_windows);
    for (auto& w : b.windows) {
        uint32_t id_len;
        get(&id_len, 4);
        w.id.resize(id_len);
        get(w.id.data(), id_len);
        get(&w.n_prefix, 4);
        get(&w.n_scored, 4);
        get(&w.hash, 8);
        w.pos.resize(size_t(w.n_scored));
        for (auto& p : w.pos) {
            get(&p.next_id, 4);
            get(&p.next_lp, 4);
            get(&p.lse, 8);
            p.top.resize(b.topk);
            get(p.top.data(), b.topk * sizeof(kld::TopEntry));
        }
    }
    if (f.peek() != EOF) throw fail(path + ": trailing data (window count mismatch)");
    return b;
}

// ---------------------------------------------------------------------------------------------
// tokenization parity with lovelace-kld

struct Tokenized {
    std::vector<ninfer::TokenId> tokens;  // n_prefix + n_scored
    int32_t n_prefix = 0, n_scored = 0;
    uint64_t hash = 0;
};

Tokenized tokenize_window(const ninfer::Engine& engine, const Window& w, int32_t n_scored) {
    Tokenized t;
    const std::string_view text(w.text);
    t.tokens          = engine.tokenize_text(text.substr(0, w.start_byte));
    const auto tail   = engine.tokenize_text(text.substr(w.start_byte));
    t.n_prefix        = int32_t(t.tokens.size());
    t.n_scored        = int32_t(std::min<size_t>(tail.size(), size_t(n_scored)));
    t.tokens.insert(t.tokens.end(), tail.begin(), tail.begin() + t.n_scored);
    static_assert(sizeof(ninfer::TokenId) == sizeof(int32_t));
    t.hash = kld::fnv1a(&t.n_prefix, sizeof(t.n_prefix));
    t.hash = kld::fnv1a(t.tokens.data(), t.tokens.size() * sizeof(ninfer::TokenId), t.hash);
    return t;
}

// "" when the window reproduces the base run's tokens, else a description of the difference.
std::string parity(const Tokenized& t, const BaseWindow& bw) {
    std::string why;
    if (t.n_prefix != bw.n_prefix)
        why += " scored-span start " + std::to_string(t.n_prefix) + " != base " + std::to_string(bw.n_prefix) + ";";
    if (t.n_scored != bw.n_scored)
        why += " scored tokens " + std::to_string(t.n_scored) + " != base " + std::to_string(bw.n_scored) + ";";
    int32_t id_diff = 0, first = -1;
    for (int32_t j = 0; j < std::min(t.n_scored, bw.n_scored); ++j) {
        if (t.tokens[size_t(t.n_prefix + j)] != bw.pos[size_t(j)].next_id) {
            ++id_diff;
            if (first < 0) first = j;
        }
    }
    if (id_diff)
        why += " " + std::to_string(id_diff) + " scored ids differ (first at +" + std::to_string(first) + ": " +
               std::to_string(t.tokens[size_t(t.n_prefix + first)]) + " vs base " +
               std::to_string(bw.pos[size_t(first)].next_id) + ");";
    if (t.hash != bw.hash) why += " token hash differs (prefix and/or reply ids);";
    return why;
}

// ---------------------------------------------------------------------------------------------
// aggregation, identical to lovelace-kld compare

struct PosResult {
    double kl, p_tail, nll_base, nll_q;
    bool top1;
};

struct Agg {
    std::vector<double> kl;
    size_t top1 = 0;
    double nll_base = 0, nll_q = 0, p_tail = 0;

    void add(const PosResult& r) {
        kl.push_back(r.kl);
        top1 += r.top1;
        nll_base += r.nll_base;
        nll_q += r.nll_q;
        p_tail += r.p_tail;
    }
    void merge(const Agg& o) {
        kl.insert(kl.end(), o.kl.begin(), o.kl.end());
        top1 += o.top1;
        nll_base += o.nll_base;
        nll_q += o.nll_q;
        p_tail += o.p_tail;
    }
    json summary() const {
        std::vector<double> s = kl;
        std::sort(s.begin(), s.end());
        const double n = double(s.size());
        double mean    = 0;
        for (double v : s) mean += v;
        mean /= n;
        return json{{"positions", s.size()},
                    {"kl_mean", mean},
                    {"kl_median", kld::quantile(s, 0.5)},
                    {"kl_p99", kld::quantile(s, 0.99)},
                    {"kl_p999", kld::quantile(s, 0.999)},
                    {"kl_max", s.back()},
                    {"top1_agree_pct", 100.0 * double(top1) / n},
                    {"nll_base", nll_base / n},
                    {"nll_q", nll_q / n},
                    {"delta_nll", (nll_q - nll_base) / n},
                    {"p_tail_mean", p_tail / n}};
    }
};

void print_row(const char* label, int label_w, const json& s) {
    printf("%-*s %7zu %9.5f %9.5f %9.4f %9.4f %7.2f %8.4f %8.4f %+9.5f %8.5f\n", label_w, label,
           s["positions"].get<size_t>(), s["kl_mean"].get<double>(), s["kl_median"].get<double>(),
           s["kl_p99"].get<double>(), s["kl_p999"].get<double>(), s["top1_agree_pct"].get<double>(),
           s["nll_base"].get<double>(), s["nll_q"].get<double>(), s["delta_nll"].get<double>(),
           s["p_tail_mean"].get<double>());
}

void print_head(const char* label, int label_w) {
    printf("%-*s %7s %9s %9s %9s %9s %7s %8s %8s %9s %8s\n", label_w, label, "pos", "KL mean", "KL med", "KL p99",
           "KL p99.9", "top1 %", "NLL ref", "NLL q", "dNLL", "p_tail");
}

double seconds_since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

size_t device_used_bytes() {
    size_t free_b = 0, total_b = 0;
    if (cudaMemGetInfo(&free_b, &total_b) != cudaSuccess) return 0;
    return total_b - free_b;
}

double gib(size_t b) { return double(b) / double(1ULL << 30); }

// ---------------------------------------------------------------------------------------------

int run(const Options& o, spdlog::logger& log, ninfer::product::StartupLogRenderer& startup_log) {
    BaseFile base = read_base(o.base);
    const auto windows = load_windows(o.windows);
    for (const auto& b : o.buckets)
        if (std::none_of(windows.begin(), windows.end(), [&](const auto& w) { return w.second.bucket == b; }))
            throw fail("--buckets: no windows in bucket " + b + " in " + o.windows);
    for (const auto& bw : base.windows)
        if (!windows.count(bw.id)) throw fail("base window " + bw.id + " not in " + o.windows);
    const size_t n_base_windows = base.windows.size();
    std::erase_if(base.windows, [&](const BaseWindow& bw) {
        return !o.buckets.empty() &&
               std::find(o.buckets.begin(), o.buckets.end(), windows.at(bw.id).bucket) == o.buckets.end();
    });
    if (base.windows.empty()) throw fail("no base windows in the selected buckets");
    const int max_scored = base.meta.value("max_scored", 1024);

    uint32_t max_context = o.max_context;
    if (max_context == 0) {
        int64_t longest = 0;
        for (const auto& bw : base.windows) longest = std::max<int64_t>(longest, bw.n_prefix + bw.n_scored);
        max_context = uint32_t((longest + 255) / 256 * 256);
    }

    if (cudaSetDevice(o.device) != cudaSuccess) throw fail("cudaSetDevice failed");
    const size_t used_before = device_used_bytes();
    ninfer::EngineOptions eo;
    eo.artifact_path    = o.artifact;
    eo.purpose          = ninfer::EnginePurpose::CausalScoring;
    eo.device           = o.device;
    eo.max_context      = max_context;
    eo.kv_cache         = o.kv;
    eo.startup_observer = startup_log.observer();
    const auto t_load   = Clock::now();
    ninfer::Engine engine(std::move(eo));
    const double load_seconds = seconds_since(t_load);
    const ninfer::LoadSummary load = engine.load_summary();
    startup_log.engine_ready(load);
    const size_t used_after_load = device_used_bytes();

    // Token parity: all windows before any scoring.
    std::vector<Tokenized> toks;
    size_t mismatched = 0;
    for (const auto& bw : base.windows) {
        toks.push_back(tokenize_window(engine, windows.at(bw.id), max_scored));
        const std::string why = parity(toks.back(), bw);
        if (!why.empty()) {
            ++mismatched;
            log.error("token parity: {}:{}", bw.id, why);
        }
    }
    if (mismatched)
        throw fail(std::to_string(mismatched) + " of " + std::to_string(base.windows.size()) +
                   " windows do not reproduce the base run's tokens");
    log.info("token parity: {} windows match the base (hash, scored-span start, next-token ids)",
             base.windows.size());
    log.info("scoring {} of {} base windows | kv {} | max_context {} | base {} (topk {})", base.windows.size(),
             n_base_windows, o.kv_name, max_context, base.meta.value("model", "?"), base.topk);

    std::vector<std::string> bucket_order;
    std::map<std::string, Agg> buckets;
    Agg overall;
    json jwindows = json::array();
    std::vector<std::pair<std::string, json>> rows;
    std::vector<float> q;  // one row of logits, float, token domain + -inf padding
    std::vector<float> host_lp;
    size_t outside_domain = 0;  // base top-k ids the engine cannot emit (padding rows)
    uint32_t token_domain = 0;  // rows the engine normalizes over
    double max_lp_diff    = 0;  // host log-softmax vs the engine's target_logprobs
    const auto t_all      = Clock::now();
    for (size_t wi = 0; wi < base.windows.size(); ++wi) {
        const BaseWindow& bw = base.windows[wi];
        const Window& w      = windows.at(bw.id);
        const Tokenized& t   = toks[wi];
        Agg agg;
        host_lp.assign(size_t(t.n_scored), NAN);
        const ninfer::ScoreLogitsSink sink = [&](const ninfer::ScoreLogitsTile& tile) {
            if (tile.physical_rows != base.n_vocab)
                throw fail("engine output rows " + std::to_string(tile.physical_rows) + " != base n_vocab " +
                           std::to_string(base.n_vocab));
            q.resize(tile.physical_rows);
            for (uint32_t c = 0; c < tile.columns; ++c) {
                const uint16_t* row = tile.bf16 + size_t(c) * tile.physical_rows;
                for (uint32_t r = 0; r < tile.valid_rows; ++r) {
                    const uint32_t bits = uint32_t(row[r]) << 16;
                    std::memcpy(&q[r], &bits, sizeof(float));
                }
                std::fill(q.begin() + tile.valid_rows, q.end(), -INFINITY);
                const uint32_t j = tile.first_target_offset + c;
                const BasePos& p = bw.pos[j];
                token_domain = tile.valid_rows;
                for (const auto& e : p.top) outside_domain += uint32_t(e.id) >= tile.valid_rows;
                const int n        = int(tile.valid_rows);
                const double q_lse = kld::logsumexp(q.data(), n);
                const auto kl      = kld::sparse_kl(p.top.data(), int(p.top.size()), q.data(), q_lse);
                const double lq    = double(q[size_t(p.next_id)]) - q_lse;
                host_lp[j]         = float(lq);
                agg.add({kl.kl, kl.p_tail, -double(p.next_lp), -lq, kld::argmax(q.data(), n) == p.top[0].id});
            }
        };
        const auto t0 = Clock::now();
        const std::vector<float> lp = engine.score_tokens(t.tokens, uint32_t(t.n_prefix), sink);
        const double dt = seconds_since(t0);
        if (agg.kl.size() != size_t(t.n_scored) || lp.size() != size_t(t.n_scored))
            throw fail(bw.id + ": engine returned " + std::to_string(agg.kl.size()) + " logit rows, " +
                       std::to_string(lp.size()) + " logprobs for " + std::to_string(t.n_scored) + " targets");
        for (size_t j = 0; j < lp.size(); ++j) max_lp_diff = std::max(max_lp_diff, double(std::fabs(lp[j] - host_lp[j])));
        log.info("[{}/{}] {}: depth {}, scored {}, {:.1f}s ({:.0f} tok/s), KL mean {:.5f}", wi + 1,
                 base.windows.size(), bw.id, t.n_prefix, t.n_scored, dt, double(t.tokens.size()) / dt,
                 agg.summary()["kl_mean"].get<double>());
        json s = agg.summary();
        rows.emplace_back(bw.id, s);
        jwindows.push_back(json{{"id", bw.id},
                                {"session", w.session},
                                {"depth_bucket", w.bucket},
                                {"depth_tokens", t.n_prefix},
                                {"scored_tokens", t.n_scored},
                                {"seconds", dt},
                                {"stats", s}});
        if (!buckets.count(w.bucket)) bucket_order.push_back(w.bucket);
        buckets[w.bucket].merge(agg);
        overall.merge(agg);
    }
    const double score_seconds = seconds_since(t_all);
    const size_t used_end      = device_used_bytes();
    const ninfer::MemorySummary mem = engine.memory_summary();
    if (outside_domain)
        log.warn("{} base top-k entries lie outside the engine's token domain (q = 0 there)", outside_domain);

    char when[32];
    const time_t now = time(nullptr);
    strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    const json run{{"engine", "ninfer"},
                   {"model", o.artifact},
                   {"windows", o.windows},
                   {"buckets", o.buckets},
                   {"n_ctx", max_context},
                   {"ctk", o.kv_name},
                   {"ctv", o.kv_name},
                   {"kv_dtype", o.kv_name},
                   {"prefill_chunk", 1024},
                   {"logits", "bf16, normalized over token ids [0," + std::to_string(token_domain) + ")"},
                   {"topk", base.topk},
                   {"max_scored", max_scored},
                   {"created", when}};
    json jb = json::object();
    for (const auto& b : bucket_order) jb[b] = buckets[b].summary();
    const json jo = overall.summary();
    const json result{
        {"model", o.artifact},
        {"model_hash", kld::fnv1a(o.artifact.data(), o.artifact.size())},
        {"run", run},
        {"base", {{"path", o.base}, {"model_hash", base.model_hash}, {"topk", base.topk}, {"meta", base.meta}}},
        {"ninfer",
         {{"target", load.target},
          {"model_id", load.model_id},
          {"weights_id", load.weights_id},
          {"token_parity_windows", base.windows.size()},
          {"base_topk_outside_token_domain", outside_domain},
          {"max_abs_logprob_diff_host_vs_engine", max_lp_diff},
          {"load_seconds", load_seconds},
          {"score_seconds", score_seconds},
          {"device_used_before_load_gib", gib(used_before)},
          {"device_used_after_load_gib", gib(used_after_load)},
          {"device_used_end_gib", gib(used_end)},
          {"weights_gib", gib(mem.weights.capacity_bytes)},
          {"sequence_gib", gib(mem.sequence.capacity_bytes)},
          {"workspace_gib", gib(mem.workspace.capacity_bytes)},
          {"workspace_peak_gib", gib(mem.workspace.peak_used_bytes)},
          {"text_kv_gib", gib(mem.text_kv_bytes)},
          {"gdn_state_gib", gib(mem.gdn_state_bytes)}}},
        {"overall", jo},
        {"buckets", jb},
        {"windows", jwindows}};
    const std::string part = o.out + ".part";
    {
        std::ofstream f(part);
        if (!f) throw fail("cannot write " + part);
        f << result.dump(2) << "\n";
        if (!f) throw fail("write failed: " + part);
    }
    if (std::rename(part.c_str(), o.out.c_str()) != 0) throw fail("cannot rename " + part);

    size_t idw = 6;
    for (const auto& r : rows) idw = std::max(idw, r.first.size());
    idw = std::min<size_t>(idw, 60);
    printf("\n%s (kv %s)  vs base %s  (load %.0fs, score %.0fs; device used %.2f GiB after load, %.2f GiB at end; "
           "KV %.2f GiB)\n\n",
           o.artifact.c_str(), o.kv_name.c_str(), base.meta.value("model", "?").c_str(), load_seconds, score_seconds,
           gib(used_after_load), gib(used_end), gib(mem.text_kv_bytes));
    print_head("window", int(idw));
    for (const auto& r : rows) print_row(r.first.substr(0, idw).c_str(), int(idw), r.second);
    printf("\n");
    print_head("bucket", 8);
    for (const auto& b : bucket_order) print_row(b.c_str(), 8, jb[b]);
    print_row("overall", 8, jo);
    log.info("wrote {} (host vs engine target log-prob max |diff| {:.2e})", o.out, max_lp_diff);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    try {
        if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
            fputs(USAGE, stdout);
            return 0;
        }
        o = parse_options(argc, argv);
    } catch (const std::exception& e) {
        fprintf(stderr, "ninfer-kld: %s\n%s", e.what(), USAGE);
        return 2;
    }
    ninfer::product::LoggingRuntime logging({.logger_name  = "ninfer-kld",
                                             .level        = o.log_level,
                                             .presentation = ninfer::product::LogPresentation::Tool});
    ninfer::product::StartupLogRenderer startup_log(logging);
    try {
        return run(o, *logging.logger(), startup_log);
    } catch (const std::exception& e) {
        logging.logger()->error("{}", e.what());
        return 1;
    }
}
