// Host tool: writes the packaged default config.json and config.schema.json
// into a mod package directory. Run at build time so the shipped defaults and
// the runtime schema cannot drift apart.

#include <filesystem>
#include <iostream>

#include "MasEffectConfig.hpp"

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: masceffects_config_gen <mod-package-dir>\n";
    return 2;
  }

  const std::filesystem::path packageDir(argv[1]);
  const auto configDir = packageDir / "config";

  pl::config::ConfigFile<masc::MasEffectConfig> config(
      masc::MasEffectConfig{}, configDir / "config.json",
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
