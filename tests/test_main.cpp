#include <gtest/gtest.h>

#include "core/log.hpp"
#include "platform/fs.hpp"

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  // Quiet by default; GPUOC_TEST_LOG=debug|info|warn shows more.
  const std::string lvl = gpuoc::platform::getenv_utf8("GPUOC_TEST_LOG");
  gpuoc::log::set_level(lvl == "debug"  ? gpuoc::log::Level::Debug
                        : lvl == "info" ? gpuoc::log::Level::Info
                        : lvl == "warn" ? gpuoc::log::Level::Warn
                                        : gpuoc::log::Level::Off);
  return RUN_ALL_TESTS();
}
