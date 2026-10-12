// mpq_extract DATA_DIR MASK OUT_DIR: copy every archive file matching MASK (e.g. "DBFilesClient\\*.dbc") out of the
// 1.12 archive set, highest-priority version of each, to OUT_DIR/<path with '/'>.
#include "wowcore/mpq.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

int main(int argc, char** argv) {
    if (argc != 4) { std::fprintf(stderr, "usage: mpq_extract DATA_DIR MASK OUT_DIR\n"); return 1; }
    const wowcore::Mpq mpq(argv[1]);
    int n = 0;
    for (const auto& name : mpq.list(argv[2])) {
        std::string rel = name;
        for (char& c : rel) if (c == '\\') c = '/';
        const auto bytes = mpq.read(name);
        if (!bytes) continue;
        const auto out = std::filesystem::path(argv[3]) / rel;
        std::filesystem::create_directories(out.parent_path());
        std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        ++n;
    }
    std::printf("%d files\n", n);
}
