#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kEgoContractMagic = 0x314F4745U;
constexpr std::size_t kEgoFrameHeaderSize = 72U;

struct Options {
    std::string board_base_url;
    std::string board_record_name;
    std::string source_host;
    int source_port = 22;
    std::string source_user = "admin";
    std::string source_password;
    std::string source_identity_file;
    std::string source_runtime_dir = "~/SourceSiren/runtime";
    std::string source_session_id;
    std::string scenario_id;
    std::string scenario_name;
    std::string oper;
    std::string notes;
    int repeat_number = 1;
    std::string siren_type;
    std::string runtime_root;
    std::string offline_root;
    std::string sessions_root;
    std::string off_config;
    std::string off_s3_config;
    bool skip_s3 = false;
    double source_timeout_s = 20.0;
    double board_timeout_s = 45.0;
    std::string board_state_json;
    std::string upload_state_json;
    std::string board_state_file;
    std::string upload_state_file;
    std::string test_stand_config = "board_web_localpc";
    std::string vehicle_id = "BOARD-WEB-001";
    std::string requested_session_id;
};

struct CommandResult {
    int rc = 0;
    std::string output;
};

struct FrameScanResult {
    std::uint64_t frames = 0;
    std::uint64_t first_ts_ns = 0;
    std::uint64_t last_ts_ns = 0;
};

std::string shell_quote(const std::string& value) {
    if (value.empty()) {
        return "''";
    }
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\"'\"'";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
}

std::string trim(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1U);
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20U) {
                    out << "\\u";
                    out << std::hex << std::uppercase;
                    out.width(4);
                    out.fill('0');
                    out << static_cast<int>(ch);
                    out << std::dec << std::nouppercase;
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

std::string utc_now_iso8601() {
    using Clock = std::chrono::system_clock;
    const auto now = Clock::now();
    const std::time_t t = Clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::string make_session_id() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::mt19937 rng(static_cast<std::mt19937::result_type>(ms));
    std::uniform_int_distribution<int> dist(1000, 9999);
    std::ostringstream out;
    out << "session-" << ms << "-" << dist(rng);
    return out.str();
}

void print_usage() {
    std::cerr
        << "Usage: localpc-finalize"
        << " --board-base-url <url>"
        << " --board-record-name <name>"
        << " --source-host <host>"
        << " --source-session-id <id>"
        << " [options]\n";
}

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };
        if (arg == "--board-base-url") {
            opts.board_base_url = next("--board-base-url");
        } else if (arg == "--board-record-name") {
            opts.board_record_name = next("--board-record-name");
        } else if (arg == "--source-host") {
            opts.source_host = next("--source-host");
        } else if (arg == "--source-port") {
            opts.source_port = std::stoi(next("--source-port"));
        } else if (arg == "--source-user") {
            opts.source_user = next("--source-user");
        } else if (arg == "--source-password") {
            opts.source_password = next("--source-password");
        } else if (arg == "--source-identity-file") {
            opts.source_identity_file = next("--source-identity-file");
        } else if (arg == "--source-runtime-dir") {
            opts.source_runtime_dir = next("--source-runtime-dir");
        } else if (arg == "--source-session-id") {
            opts.source_session_id = next("--source-session-id");
        } else if (arg == "--scenario-id") {
            opts.scenario_id = next("--scenario-id");
        } else if (arg == "--scenario-name") {
            opts.scenario_name = next("--scenario-name");
        } else if (arg == "--operator") {
            opts.oper = next("--operator");
        } else if (arg == "--notes") {
            opts.notes = next("--notes");
        } else if (arg == "--repeat-number") {
            opts.repeat_number = std::stoi(next("--repeat-number"));
        } else if (arg == "--siren-type") {
            opts.siren_type = next("--siren-type");
        } else if (arg == "--runtime-root") {
            opts.runtime_root = next("--runtime-root");
        } else if (arg == "--offline-root") {
            opts.offline_root = next("--offline-root");
        } else if (arg == "--sessions-root") {
            opts.sessions_root = next("--sessions-root");
        } else if (arg == "--off-config") {
            opts.off_config = next("--off-config");
        } else if (arg == "--off-s3-config") {
            opts.off_s3_config = next("--off-s3-config");
        } else if (arg == "--skip-s3") {
            opts.skip_s3 = true;
        } else if (arg == "--source-timeout-s") {
            opts.source_timeout_s = std::stod(next("--source-timeout-s"));
        } else if (arg == "--board-timeout-s") {
            opts.board_timeout_s = std::stod(next("--board-timeout-s"));
        } else if (arg == "--board-state-json") {
            opts.board_state_json = next("--board-state-json");
        } else if (arg == "--upload-state-json") {
            opts.upload_state_json = next("--upload-state-json");
        } else if (arg == "--board-state-file") {
            opts.board_state_file = next("--board-state-file");
        } else if (arg == "--upload-state-file") {
            opts.upload_state_file = next("--upload-state-file");
        } else if (arg == "--test-stand-config") {
            opts.test_stand_config = next("--test-stand-config");
        } else if (arg == "--vehicle-id") {
            opts.vehicle_id = next("--vehicle-id");
        } else if (arg == "--requested-session-id") {
            opts.requested_session_id = next("--requested-session-id");
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    if (opts.board_base_url.empty() || opts.board_record_name.empty() ||
        opts.source_host.empty() || opts.source_session_id.empty()) {
        throw std::runtime_error("required arguments are missing");
    }
    return opts;
}

CommandResult run_command_capture(const std::string& command) {
    const std::string wrapped = command + " 2>&1";
#if defined(_WIN32)
    FILE* pipe = _popen(wrapped.c_str(), "r");
#else
    FILE* pipe = popen(wrapped.c_str(), "r");
#endif
    if (pipe == nullptr) {
        throw std::runtime_error("failed to execute command");
    }

    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output += buffer;
    }
#if defined(_WIN32)
    const int rc = _pclose(pipe);
#else
    const int rc = pclose(pipe);
#endif
    return {rc, trim(output)};
}

void ensure_ok(const CommandResult& result, const std::string& context) {
    if (result.rc != 0) {
        throw std::runtime_error(context + ": " + (result.output.empty() ? "command failed" : result.output));
    }
}

std::unordered_map<std::string, std::string> parse_key_values(const std::string& text) {
    std::unordered_map<std::string, std::string> out;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        out.emplace(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
    return out;
}

std::string expand_home(std::string value) {
    if (value == "~") {
        const char* home = std::getenv("HOME");
        return home == nullptr ? value : std::string(home);
    }
    if (value.rfind("~/", 0) == 0) {
        const char* home = std::getenv("HOME");
        if (home == nullptr) {
            return value;
        }
        return (fs::path(home) / value.substr(2)).string();
    }
    return value;
}

std::string ssh_prefix(const Options& opts) {
    std::ostringstream cmd;
    if (!opts.source_password.empty()) {
        cmd << "sshpass -p " << shell_quote(opts.source_password) << " ";
    }
    cmd << "ssh -p " << opts.source_port << " -o StrictHostKeyChecking=accept-new ";
    if (!opts.source_identity_file.empty()) {
        cmd << "-i " << shell_quote(expand_home(opts.source_identity_file))
            << " -o PasswordAuthentication=no ";
    }
    cmd << shell_quote(opts.source_user + "@" + opts.source_host);
    return cmd.str();
}

std::string scp_prefix(const Options& opts) {
    std::ostringstream cmd;
    if (!opts.source_password.empty()) {
        cmd << "sshpass -p " << shell_quote(opts.source_password) << " ";
    }
    cmd << "scp -P " << opts.source_port << " -o StrictHostKeyChecking=accept-new ";
    if (!opts.source_identity_file.empty()) {
        cmd << "-i " << shell_quote(expand_home(opts.source_identity_file))
            << " -o PasswordAuthentication=no ";
    }
    return cmd.str();
}

std::string runtime_dir_shell(const std::string& path) {
    const std::string trimmed = trim(path);
    if (trimmed == "~") {
        return "$HOME";
    }
    if (trimmed.rfind("~/", 0) == 0) {
        return "$HOME/" + shell_quote(trimmed.substr(2));
    }
    return shell_quote(trimmed);
}

fs::path create_session_dir(const fs::path& sessions_root, const std::string& requested_session_id, std::string* effective_session_id) {
    std::string session_id = requested_session_id.empty() ? make_session_id() : requested_session_id;
    fs::path dir = sessions_root / session_id;
    if (fs::exists(dir)) {
        session_id = make_session_id();
        dir = sessions_root / session_id;
    }
    fs::create_directories(dir / "raw");
    if (effective_session_id != nullptr) {
        *effective_session_id = session_id;
    }
    return dir;
}

fs::path download_board_raw(const Options& opts, const fs::path& raw_dir) {
    const std::string query_name = opts.board_record_name;
    const std::string url = opts.board_base_url + "/api/uploads/local?name=" + query_name;
    const fs::path target = raw_dir / opts.board_record_name;
    std::ostringstream cmd;
    cmd << "curl -fsSL " << shell_quote(url)
        << " -o " << shell_quote(target.string());
    ensure_ok(run_command_capture(cmd.str()), "board raw download failed");
    return target;
}

fs::path fetch_source_bin(const Options& opts, const fs::path& session_dir) {
    const std::string export_cmd =
        "cd " + runtime_dir_shell(opts.source_runtime_dir) +
        " && ./run.sh export --session-id " + shell_quote(opts.source_session_id);
    const std::string remote_cmd =
        ssh_prefix(opts) + " " + shell_quote("bash -lc " + shell_quote(export_cmd));
    const auto export_result = run_command_capture(remote_cmd);
    ensure_ok(export_result, "SourceSiren export failed");

    const auto parsed = parse_key_values(export_result.output);
    const auto it = parsed.find("artifact");
    if (it == parsed.end() || it->second.empty()) {
        throw std::runtime_error("SourceSiren export did not return artifact path");
    }

    const fs::path local_target = session_dir / "source.bin";
    std::ostringstream scp;
    scp << scp_prefix(opts)
        << shell_quote(opts.source_user + "@" + opts.source_host + ":" + it->second)
        << " " << shell_quote(local_target.string());
    ensure_ok(run_command_capture(scp.str()), "source.bin download failed");
    return local_target;
}

FrameScanResult scan_contract_frames(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        throw std::runtime_error("cannot open raw log");
    }
    std::uint32_t magic = 0;
    input.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != kEgoContractMagic) {
        throw std::runtime_error("raw log is not ego-contract EGO1");
    }
    input.seekg(0);

    FrameScanResult result{};
    std::vector<char> header(kEgoFrameHeaderSize);
    while (input.read(header.data(), static_cast<std::streamsize>(header.size()))) {
        std::uint32_t payload_size = 0;
        std::uint64_t t0 = 0;
        std::uint64_t t1 = 0;
        std::memcpy(&payload_size, header.data() + 56, sizeof(payload_size));
        std::memcpy(&t0, header.data() + 40, sizeof(t0));
        std::memcpy(&t1, header.data() + 48, sizeof(t1));
        if (result.frames == 0) {
            result.first_ts_ns = t0;
        }
        result.last_ts_ns = t1;
        ++result.frames;
        input.seekg(static_cast<std::streamoff>(payload_size), std::ios::cur);
    }
    return result;
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string read_text_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        throw std::runtime_error("cannot open file: " + path.string());
    }
    std::ostringstream data;
    data << input.rdbuf();
    return data.str();
}

void write_json_passthrough(const fs::path& path, const std::string& json_text) {
    if (trim(json_text).empty()) {
        return;
    }
    write_text(path, json_text + "\n");
}

void write_session_metadata(const fs::path& session_dir, const std::string& session_id, const Options& opts) {
    std::ostringstream json;
    json << "{\n"
         << "  \"session_id\": \"" << json_escape(session_id) << "\",\n"
         << "  \"started_at_utc\": \"" << utc_now_iso8601() << "\",\n"
         << "  \"stopped_at_utc\": \"" << utc_now_iso8601() << "\",\n"
         << "  \"vehicle_id\": \"" << json_escape(opts.vehicle_id) << "\",\n"
         << "  \"test_stand_config\": \"" << json_escape(opts.test_stand_config) << "\",\n"
         << "  \"software_version\": \"localpc-finalize\",\n"
         << "  \"runtime_version\": \"localpc-finalize\",\n"
         << "  \"protocol_version\": \"1\",\n"
         << "  \"prod_protocol_version\": 1,\n"
         << "  \"source_ip\": \"" << json_escape(opts.source_host) << "\",\n"
         << "  \"storage_path\": \"" << json_escape(session_dir.string()) << "\",\n"
         << "  \"stop_reason\": \"board_source_finalize\"\n"
         << "}\n";
    write_text(session_dir / "session_metadata.json", json.str());
}

void write_scenario_metadata(const fs::path& session_dir, const Options& opts) {
    std::ostringstream json;
    json << "{\n"
         << "  \"scenario_id\": \"" << json_escape(opts.scenario_id) << "\",\n"
         << "  \"scenario_name\": \"" << json_escape(opts.scenario_name) << "\",\n"
         << "  \"operator\": \"" << json_escape(opts.oper) << "\",\n"
         << "  \"notes\": \"" << json_escape(opts.notes) << "\",\n"
         << "  \"repeat_number\": " << opts.repeat_number << ",\n"
         << "  \"siren_type\": \"" << json_escape(opts.siren_type) << "\"\n"
         << "}\n";
    write_text(session_dir / "scenario_metadata.json", json.str());
    write_text(session_dir / "gui_scenario_run.json", json.str());
}

void install_contract_bundle(const fs::path& session_dir, const fs::path& raw_path, const std::string& session_id, const Options& opts, FrameScanResult* scan_out) {
    const fs::path ego_path = session_dir / "ego.bin";
    fs::copy_file(raw_path, ego_path, fs::copy_options::overwrite_existing);
    const auto scan = scan_contract_frames(ego_path);
    if (scan_out != nullptr) {
        *scan_out = scan;
    }
    std::ostringstream manifest;
    manifest << "{\n"
             << "  \"session_id\": \"" << json_escape(session_id) << "\",\n"
             << "  \"first_ts_ns\": " << scan.first_ts_ns << ",\n"
             << "  \"last_ts_ns\": " << scan.last_ts_ns << ",\n"
             << "  \"packet_count\": " << scan.frames << ",\n"
             << "  \"chunks\": [\n"
             << "    {\n"
             << "      \"chunk_id\": 0,\n"
             << "      \"file\": \"ego.bin\",\n"
             << "      \"bytes\": " << fs::file_size(ego_path) << ",\n"
             << "      \"first_ts_ns\": " << scan.first_ts_ns << ",\n"
             << "      \"last_ts_ns\": " << scan.last_ts_ns << ",\n"
             << "      \"packet_count\": " << scan.frames << "\n"
             << "    }\n"
             << "  ]\n"
             << "}\n";
    write_text(session_dir / "ego_manifest.json", manifest.str());
    write_session_metadata(session_dir, session_id, opts);
}

CommandResult run_offline_process(const Options& opts, const fs::path& offline_root, const fs::path& session_dir) {
    std::ostringstream cmd;
    cmd << shell_quote((offline_root / "run.sh").string())
        << " process --session-dir " << shell_quote(session_dir.string());
    if (!opts.off_config.empty()) {
        cmd << " --config " << shell_quote(expand_home(opts.off_config));
    }
    if (!opts.off_s3_config.empty()) {
        cmd << " --s3-config " << shell_quote(expand_home(opts.off_s3_config));
    }
    if (opts.skip_s3) {
        cmd << " --skip-s3";
    }
    return run_command_capture(cmd.str());
}

std::string extract_json_string(const std::string& raw, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    const auto pos = raw.find(marker);
    if (pos == std::string::npos) {
        return {};
    }
    const auto colon = raw.find(':', pos);
    if (colon == std::string::npos) {
        return {};
    }
    auto start = raw.find('"', colon + 1U);
    if (start == std::string::npos) {
        return {};
    }
    ++start;
    std::string out;
    for (std::size_t i = start; i < raw.size(); ++i) {
        const char ch = raw[i];
        if (ch == '\\' && i + 1U < raw.size()) {
            out.push_back(raw[i + 1U]);
            ++i;
            continue;
        }
        if (ch == '"') {
            return out;
        }
        out.push_back(ch);
    }
    return {};
}

std::string read_upload_status(const fs::path& session_dir) {
    const fs::path report_path = session_dir / "offline" / "upload_report.json";
    if (!fs::exists(report_path)) {
        return {};
    }
    std::ifstream input(report_path);
    std::ostringstream raw;
    raw << input.rdbuf();
    return extract_json_string(raw.str(), "upload_status");
}

std::string s3_status_from(int rc, const std::string& upload_status) {
    if (!upload_status.empty()) {
        return upload_status;
    }
    if (rc == 5) {
        return "blocked";
    }
    if (rc == 4) {
        return "failed";
    }
    if (rc == 0) {
        return "not_run";
    }
    return "unknown";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options opts = parse_args(argc, argv);
        const fs::path runtime_root = expand_home(opts.runtime_root);
        const fs::path offline_root = expand_home(opts.offline_root);
        const fs::path sessions_root = expand_home(opts.sessions_root);

        std::string session_id;
        const fs::path session_dir = create_session_dir(sessions_root, opts.requested_session_id, &session_id);
        const fs::path raw_dir = session_dir / "raw";

        const auto t_start = std::chrono::steady_clock::now();
        const fs::path raw_path = download_board_raw(opts, raw_dir);
        const auto t_after_raw = std::chrono::steady_clock::now();
        const fs::path source_path = fetch_source_bin(opts, session_dir);
        const auto t_after_source = std::chrono::steady_clock::now();
        const std::string board_state_json = !opts.board_state_file.empty()
            ? read_text_file(expand_home(opts.board_state_file))
            : opts.board_state_json;
        const std::string upload_state_json = !opts.upload_state_file.empty()
            ? read_text_file(expand_home(opts.upload_state_file))
            : opts.upload_state_json;

        write_scenario_metadata(session_dir, opts);
        write_json_passthrough(session_dir / "board_session_state.json", board_state_json);
        write_json_passthrough(session_dir / "board_upload_state.json", upload_state_json);

        FrameScanResult scan{};
        install_contract_bundle(session_dir, raw_path, session_id, opts, &scan);
        const auto t_after_bundle = std::chrono::steady_clock::now();

        const auto offline_result = run_offline_process(opts, offline_root, session_dir);
        const auto t_after_offline = std::chrono::steady_clock::now();
        const std::string upload_status = read_upload_status(session_dir);
        const std::string s3_status = s3_status_from(offline_result.rc, upload_status);

        auto elapsed = [](auto a, auto b) {
            return std::chrono::duration<double>(b - a).count();
        };

        const fs::path report_path = session_dir / "localpc_finalize_report.json";
        std::ostringstream report;
        report << "{\n"
               << "  \"status\": \"" << (offline_result.rc == 0 || offline_result.rc == 4 || offline_result.rc == 5 ? "ok" : "error") << "\",\n"
               << "  \"session_id\": \"" << json_escape(session_id) << "\",\n"
               << "  \"session_dir\": \"" << json_escape(session_dir.string()) << "\",\n"
               << "  \"board_record_name\": \"" << json_escape(opts.board_record_name) << "\",\n"
               << "  \"raw_path\": \"" << json_escape(raw_path.string()) << "\",\n"
               << "  \"source_path\": \"" << json_escape(source_path.string()) << "\",\n"
               << "  \"ego_frames\": " << scan.frames << ",\n"
               << "  \"off_process_rc\": " << offline_result.rc << ",\n"
               << "  \"s3_status\": \"" << json_escape(s3_status) << "\",\n"
               << "  \"upload_report_path\": \"" << json_escape((session_dir / "offline" / "upload_report.json").string()) << "\",\n"
               << "  \"timings\": {\n"
               << "    \"download_raw_s\": " << elapsed(t_start, t_after_raw) << ",\n"
               << "    \"download_source_s\": " << elapsed(t_after_raw, t_after_source) << ",\n"
               << "    \"install_contract_bundle_s\": " << elapsed(t_after_source, t_after_bundle) << ",\n"
               << "    \"offline_process_s\": " << elapsed(t_after_bundle, t_after_offline) << "\n"
               << "  }\n"
               << "}\n";
        write_text(report_path, report.str());

        std::cout << "status=" << (offline_result.rc == 0 || offline_result.rc == 4 || offline_result.rc == 5 ? "ok" : "error") << "\n";
        std::cout << "session_id=" << session_id << "\n";
        std::cout << "session_dir=" << session_dir.string() << "\n";
        std::cout << "report_path=" << report_path.string() << "\n";
        std::cout << "raw_path=" << raw_path.string() << "\n";
        std::cout << "source_path=" << source_path.string() << "\n";
        std::cout << "s3_status=" << s3_status << "\n";
        std::cout << "upload_report_path=" << (session_dir / "offline" / "upload_report.json").string() << "\n";
        std::cout << "timings={\"download_raw_s\":" << elapsed(t_start, t_after_raw)
                  << ",\"download_source_s\":" << elapsed(t_after_raw, t_after_source)
                  << ",\"install_contract_bundle_s\":" << elapsed(t_after_source, t_after_bundle)
                  << ",\"offline_process_s\":" << elapsed(t_after_bundle, t_after_offline) << "}\n";
        if (offline_result.rc != 0 && offline_result.rc != 4 && offline_result.rc != 5) {
            std::cout << "message=" << json_escape(offline_result.output) << "\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cout << "status=error\n";
        std::cout << "message=" << e.what() << "\n";
        return 1;
    }
}
