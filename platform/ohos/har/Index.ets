// Public ArkTS entry of the HAR: re-export the capability probes and the
// lifecycle of the tty/exec services so consumers can `import { ... } from 'iec'`.
import { hello, setCaps, probeCaps, startIecServer, stopIecServer } from 'libiec_server.so';

export { hello, setCaps, probeCaps, startIecServer, stopIecServer };
