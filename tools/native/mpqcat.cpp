// mpqcat <archive.mpq>[,<patch.mpq>...] <file> <out>: extract one file with StormLib (the reference
// MPQ implementation). Later archives in the comma list take precedence (patch chain).
#include <StormLib.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	if (argc != 4) {
		std::fprintf(stderr, "usage: mpqcat <a.mpq>[,<b.mpq>...] <file> <out>\n");
		return 2;
	}
	std::vector<std::string> archives;
	std::stringstream ss(argv[1]);
	for (std::string a; std::getline(ss, a, ',');) {
		archives.push_back(a);
	}
	for (auto it = archives.rbegin(); it != archives.rend(); ++it) {  // highest priority first
		HANDLE mpq = nullptr;
		if (!SFileOpenArchive(it->c_str(), 0, STREAM_FLAG_READ_ONLY, &mpq)) {
			std::fprintf(stderr, "open failed %s: %d\n", it->c_str(), SErrGetLastError());
			return 1;
		}
		if (SFileHasFile(mpq, argv[2])) {
			const bool ok = SFileExtractFile(mpq, argv[2], argv[3], SFILE_OPEN_FROM_MPQ);
			std::printf("%s from %s: %s\n", argv[2], it->c_str(), ok ? "ok" : "FAILED");
			SFileCloseArchive(mpq);
			return ok ? 0 : 1;
		}
		SFileCloseArchive(mpq);
	}
	std::fprintf(stderr, "not found: %s\n", argv[2]);
	return 1;
}
