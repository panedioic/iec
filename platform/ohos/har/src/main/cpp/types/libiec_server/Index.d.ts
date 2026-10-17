// ArkTS declaration for libiec_server.so (must match the exports in napi_init.cpp).
export interface Caps {
  packed: number;
  fork: number;
  exec: number;
  jit: number;
  path: number;
}

export const hello: () => string;
export const setCaps: (cap: number) => Caps;
export const probeCaps: () => Caps;
