#include "ego_offline/load/ego_nav_sidecar_reader.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    const fs::path dir = fs::temp_directory_path() / "ego_nav_sidecar_reader_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    const fs::path path = dir / "ego_nav.jsonl";
    {
        std::ofstream out(path);
        out << "{\"ts_ns\":1000,\"lat_deg\":55.0,\"lon_deg\":37.0,\"alt_m\":170.0,"
               "\"speed_mps\":0.4,\"heading_deg\":161.3,\"fix_quality\":1,"
               "\"satellites\":7,\"hdop\":2.1,\"source\":\"local_m2_tcp\"}\n";
        out << "{\"ts_ns\":1000,\"lat_deg\":55.1,\"lon_deg\":37.1,\"alt_m\":171.0,"
               "\"speed_mps\":0.5,\"heading_deg\":162.0,\"fix_quality\":1,"
               "\"satellites\":8,\"hdop\":1.9,\"source\":\"local_m2_tcp\"}\n";
        out << "{\"ts_ns\":2000,\"lat_deg\":55.2,\"lon_deg\":37.2,\"alt_m\":172.0,"
               "\"speed_mps\":0.6,\"heading_deg\":163.0,\"fix_quality\":1,"
               "\"satellites\":9,\"hdop\":1.8,\"source\":\"local_m2_tcp\"}\n";
        out << "{\"ts_ns\":0,\"fix_quality\":0}\n";
    }

    const auto loaded = ego_offline::load::ReadEgoNavSidecar(path);
    if (loaded.points.size() != 2U) {
        std::cerr << "FAIL: expected 2 unique valid points, got " << loaded.points.size() << '\n';
        return 1;
    }
    if (loaded.invalid_lines != 1U) {
        std::cerr << "FAIL: expected 1 invalid line, got " << loaded.invalid_lines << '\n';
        return 1;
    }
    if (loaded.source_label != "local_m2_tcp") {
        std::cerr << "FAIL: unexpected source label: " << loaded.source_label << '\n';
        return 1;
    }
    if (loaded.points.front().ts_ns != 1000 || loaded.points.back().ts_ns != 2000) {
        std::cerr << "FAIL: unexpected timestamps in loaded sidecar\n";
        return 1;
    }

    fs::remove_all(dir, ec);
    std::cout << "[PASS] ego_nav_sidecar_reader\n";
    return 0;
}
