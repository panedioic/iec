// Public ArkTS entry of the HAR: re-export the native hello() so consumers can
// `import { hello } from 'iec'`.
import { hello } from 'libiec_server.so';

export { hello };
