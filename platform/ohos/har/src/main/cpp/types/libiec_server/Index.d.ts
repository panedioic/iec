// ArkTS declaration for libiec_server.so (must match the exports in napi_init.cpp).
export interface Caps {
  packed: number;
  chmod: number;
  fork: number;
  exec: number;
  jit: number;
  path: number;
}

export const hello: () => string;
export const setCaps: (cap: number) => Caps;
export const probeCaps: () => Caps;

// Starts the tty and exec services. Omitted or empty paths use the compiled-in
// defaults. Idempotent; false means a socket could not be created.
export const startIecServer: (ttyPath?: string, execPath?: string) => boolean;

// Stops both services and joins their threads. True when a running server was
// stopped, false when none was running.
export const stopIecServer: () => boolean;

// Writes the CA bundle embedded in the library to <dir>/cacert.pem. The default
// is <fake root>/usr/share/git-core. False means the file could not be written.
export const installCaBundle: (dir?: string) => boolean;

// Registers the application's resource manager. Must be called before any
// unpackResource() call; HAR rawfiles are merged into the same manager.
export const initIecResourceManager: (resourceManager: object) => boolean;

// Returns true when <fake root>/.rawfile-version contains exactly `version`.
export const checkRawfileVersionMarker: (version: string) => boolean;

// Writes `version` to <fake root>/.rawfile-version. 0 on success.
export const writeRawfileVersionMarker: (version: string) => number;

// mkdir -p. 0 on success, 1 on failure.
export const ensureDir: (path: string) => number;

// Writes an #IECSO launcher file at `target`. 0 on success, 1 on failure.
export const writeSoLauncher: (target: string, so: string, entry: string,
                               arity: number) => number;

// Extracts a tar stored in rawfile `name` into `dest`. 0 on success, 1 on
// failure. Symlinks become <name>.IEC_SYMLINK marker files.
export const unpackResource: (name: string, dest: string) => number;
