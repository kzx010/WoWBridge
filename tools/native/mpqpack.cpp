// mpqpack <out.mpq> <root_dir>: pack every file under root_dir into a new MPQ (format v1, listfile),
// stored under its root-relative path with backslashes (e.g. DBFilesClient\Spell.dbc).
#include <StormLib.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	if (argc != 3) {
		std::fprintf(stderr, "usage: mpqpack <out.mpq> <root_dir>\n");
		return 2;
	}
	namespace fs = std::filesystem;
	std::vector<fs::path> files;
	for (auto& e : fs::recursive_directory_iterator(argv[2])) {
		if (e.is_regular_file()) {
			files.push_back(e.path());
		}
	}
	fs::remove(argv[1]);
	HANDLE mpq = nullptr;
	if (!SFileCreateArchive(argv[1], MPQ_CREATE_LISTFILE | MPQ_CREATE_ARCHIVE_V1, static_cast<DWORD>(files.size() + 16), &mpq)) {
		std::fprintf(stderr, "create failed: %d\n", SErrGetLastError());
		return 1;
	}
	int added = 0;
	for (auto& f : files) {
		auto name = fs::relative(f, argv[2]).string();
		if (name == "(listfile)" || name == "(attributes)") {
			continue;
		}
		for (auto& c : name) {
			if (c == '/') c = '\\';
		}
		if (!SFileAddFileEx(mpq, f.c_str(), name.c_str(), MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING, MPQ_COMPRESSION_ZLIB, MPQ_COMPRESSION_NEXT_SAME)) {
			std::fprintf(stderr, "add failed %s: %d\n", name.c_str(), SErrGetLastError());
			return 1;
		}
		++added;
	}
	SFileCloseArchive(mpq);
	std::printf("packed %d files into %s\n", added, argv[1]);
	return 0;
}
