#include "FpsConfig.hpp"

#include <filesystem>
#include <iostream>

// Writes the packaged config.json and config.schema.json. Built for the host so
// the shipped defaults and the schema the launcher editor reads are generated
// from the same struct the mod uses at runtime, rather than being kept in sync
// by hand.
int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: fps_opto_pro_config_gen <mod-package-dir>\n";
    return 2;
  }

  const std::filesystem::path packageDir(argv[1]);
  const auto configDir = packageDir / "config";
  pl::config::ConfigFile<fpsopto::FpsConfig> config(
      fpsopto::FpsConfig{}, configDir / "config.json",
      configDir / "config.schema.json");

  if (!config.save()) {
    std::cerr << "failed to write " << config.configPath() << '\n';
    return 1;
  }
  if (!config.writeSchema()) {
    std::cerr << "failed to write " << config.schemaPath() << '\n';
    return 1;
  }

  std::cout << "generated " << config.configPath() << '\n';
  std::cout << "generated " << config.schemaPath() << '\n';
  return 0;
}