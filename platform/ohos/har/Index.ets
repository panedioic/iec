// Public ArkTS entry of the HAR: re-export the native hello() and the
// capability wrappers so consumers can `import { ... } from 'iec'`.
import { hello, setCaps, probeCaps } from 'libiec_server.so';

export { hello, setCaps, probeCaps };
