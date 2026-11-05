// What the build defines in bundles (cmake/TypeScript.cmake, axe_ts_bundle).

/** The repository's root, for the examples' test fonts. */
declare const AXE_SOURCE_DIR: string;

/** True only in hot reload's modules (docs/ui.md, "Hot reload"): every bundle defines it false,
 * so what it guards is removed as dead code. */
declare const AXE_DEV: boolean;
