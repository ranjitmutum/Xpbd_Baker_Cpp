#include "xpbd/export/atomic_file_writer.hpp"

#include <fstream>
#include <random>
#include <stdexcept>

namespace xpbd::export_ {

void AtomicFileWriter::writeUtf8(const std::filesystem::path& file_path,
                                 const std::string& content) {
    if (file_path.empty()) {
        throw std::invalid_argument("output file path must not be blank");
    }
    const auto target = std::filesystem::absolute(file_path);
    const auto parent = target.parent_path();
    if (parent.empty() || !std::filesystem::is_directory(parent)) {
        throw std::runtime_error("Output directory does not exist: " + parent.string());
    }

    const auto stem = target.filename().string();
    std::filesystem::path temporary;
    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<unsigned long long> dist;
        temporary = parent / (stem + "." + std::to_string(dist(gen)) + ".tmp");
    }

    bool replaced = false;
    try {
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) {
                throw std::runtime_error("Failed to create temporary file: " +
                                         temporary.string());
            }
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!out) {
                throw std::runtime_error("Failed to write temporary file: " +
                                         temporary.string());
            }
        }
        std::error_code ec;
        std::filesystem::rename(temporary, target, ec);
        if (ec) {
            std::filesystem::copy_file(temporary, target,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                throw std::runtime_error("Failed to replace output file: " + ec.message());
            }
            std::filesystem::remove(temporary, ec);
        }
        replaced = true;
    } catch (...) {
        if (!replaced) {
            std::error_code ignore;
            std::filesystem::remove(temporary, ignore);
        }
        throw;
    }
}

}
