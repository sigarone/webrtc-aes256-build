// Writes the seed corpus: corpus_gen <output dir>. Run it after changing the schema, then commit
// the result (the test_corpus test fails while the committed files differ).
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "corpus_data.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: corpus_gen <output dir>\n");
    return 2;
  }
  namespace fs = std::filesystem;
  const fs::path root = argv[1];
  size_t n = 0;
  for (const auto& f : qmedia::testgen::BuildCorpus()) {
    const fs::path dir = root / f.subdir;
    fs::create_directories(dir);
    std::ofstream o(dir / f.name, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(f.bytes.data()), static_cast<std::streamsize>(f.bytes.size()));
    if (!o) {
      std::fprintf(stderr, "write failed: %s\n", f.name.c_str());
      return 1;
    }
    ++n;
  }
  std::fprintf(stderr, "wrote %zu files\n", n);
  return 0;
}
