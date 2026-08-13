#include <frametee/game_abi.h>

FT_GAME_EXPORT const ft_game_module *ft_game_module_entry(uint32_t engine_abi_version) {
  (void)engine_abi_version;
  // Validation rejects the version before reaching required callbacks.
  static const ft_game_module test_module = {
      .struct_size = sizeof(ft_game_module),
      .abi_version = FT_GAME_ABI_VERSION,
      .abi_revision = FT_GAME_ABI_REVISION,
      .info = {
          .struct_size = sizeof(ft_game_info),
          .id = "invalid-semver-test",
          .display_name = "Invalid SemVer test module",
          .version = "1.0.0-rc!1",
          .author = "FrameTee tests",
      },
  };
  return &test_module;
}
