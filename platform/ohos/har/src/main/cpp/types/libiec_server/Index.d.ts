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
