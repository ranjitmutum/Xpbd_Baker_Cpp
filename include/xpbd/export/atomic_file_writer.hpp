#pragma once

#include <filesystem>
#include <string>

namespace xpbd::export_ {


class AtomicFileWriter {
public:
    static void writeUtf8(const std::filesystem::path& file_path, const std::string& content);
};

}
