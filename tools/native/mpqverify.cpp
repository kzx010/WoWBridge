// mpqverify <archive.mpq>...: read every listed file with StormLib; report files that fail to read
// fully (damaged sectors / bad compression) and StormLib's own CRC/MD5 verification result.
#include <StormLib.h>

#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
	int totalBad = 0;
	for (int a = 1; a < argc; ++a) {
		HANDLE mpq = nullptr;
		if (!SFileOpenArchive(argv[a], 0, STREAM_FLAG_READ_ONLY, &mpq)) {
			std::printf("%s: OPEN FAILED %d\n", argv[a], SErrGetLastError());
			++totalBad;
			continue;
		}
		SFILE_FIND_DATA fd{};
		HANDLE find = SFileFindFirstFile(mpq, "*", &fd, nullptr);
		int files = 0, bad = 0;
		std::vector<char> buf;
		while (find) {
			++files;
			HANDLE f = nullptr;
			bool ok = SFileOpenFileEx(mpq, fd.cFileName, SFILE_OPEN_FROM_MPQ, &f);
			if (ok) {
				const DWORD size = SFileGetFileSize(f, nullptr);
				buf.resize(size ? size : 1);
				DWORD read = 0;
				ok = SFileReadFile(f, buf.data(), size, &read, nullptr) && read == size;
				SFileCloseFile(f);
			}
			const DWORD v = SFileVerifyFile(mpq, fd.cFileName, SFILE_VERIFY_ALL);
			const bool verifyBad = v & (VERIFY_OPEN_ERROR | VERIFY_READ_ERROR | VERIFY_FILE_SECTOR_CRC_ERROR | VERIFY_FILE_CHECKSUM_ERROR | VERIFY_FILE_MD5_ERROR);
			if (!ok || verifyBad) {
				++bad;
				if (bad <= 25) {
					std::printf("  BAD %s (read %s, verify 0x%X, err %d)\n", fd.cFileName, ok ? "ok" : "FAIL", v, SErrGetLastError());
				}
			}
			if (!SFileFindNextFile(find, &fd)) {
				break;
			}
		}
		if (find) {
			SFileFindClose(find);
		}
		SFileCloseArchive(mpq);
		std::printf("%s: %d files, %d bad\n", argv[a], files, bad);
		totalBad += bad;
	}
	return totalBad ? 1 : 0;
}
