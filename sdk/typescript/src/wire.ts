// Binary wire protocol implementation for AxiomVM appliance communication.

export const PROTOCOL_MAGIC = 0x4158494f;
export const PROTOCOL_VERSION = 0x0001;
export const HEADER_SIZE = 20;

export const EXEC_FLAG_STRIP_ANSI = 1 << 0;
export const EXEC_FLAG_CAPTURE_DIFF = 1 << 1;

export enum Opcode {
  PING = 0x0001,
  PONG = 0x0002,
  EXEC = 0x0010,
  EXEC_RESPONSE = 0x0011,
  AST_SYMBOLS = 0x0020,
  AST_SYMBOLS_RESPONSE = 0x0021,
  AST_SLICE = 0x0022,
  AST_SLICE_RESPONSE = 0x0023,
  AST_PATCH = 0x0024,
  AST_PATCH_RESPONSE = 0x0025,
  CDP_ACTION = 0x0030,
  CDP_ACTION_RESPONSE = 0x0031,
}

export enum CdpActionType {
  NAVIGATE = 0,
  CLICK = 1,
  TYPE_TEXT = 2,
  SCROLL = 3,
  GET_TREE = 4,
}

export interface Header {
  magic: number;
  version: number;
  opcode: Opcode;
  requestId: bigint;
  payloadLen: number;
}

export interface ExecRequest {
  command: string;
  cwd?: string;
  timeoutMs?: number;
  stripAnsi?: boolean;
  captureDiff?: boolean;
}

export interface ExecResponse {
  exitCode: number;
  flags: number;
  cpuCycles: bigint;
  output: string;
  diff: string;
}

export interface AstSymbolsResponse {
  status: number;
  symbolCount: number;
  symbolsText: string;
}

export interface AstSliceResponse {
  status: number;
  startLine: number;
  endLine: number;
  content: string;
}

export interface AstPatchResponse {
  status: number;
  oldStartLine: number;
  oldEndLine: number;
  newEndLine: number;
}

export interface CdpActionRequest {
  action: CdpActionType;
  flags?: number;
  targetId?: number;
  scrollDelta?: number;
  payload1?: string;
  payload2?: string;
}

export interface CdpActionResponse {
  status: number;
  nodeCount: number;
  treeText: string;
}

// Encodes a standard 20-byte wire protocol header.
export function encodeHeader(
  opcode: Opcode,
  requestId: bigint,
  payloadLen: number
): Buffer {
  const buf = Buffer.alloc(HEADER_SIZE);
  buf.writeUInt32LE(PROTOCOL_MAGIC, 0);
  buf.writeUInt16LE(PROTOCOL_VERSION, 4);
  buf.writeUInt16LE(opcode, 6);
  buf.writeBigUInt64LE(requestId, 8);
  buf.writeUInt32LE(payloadLen, 16);
  return buf;
}

// Decodes a 20-byte wire protocol header from buffer.
export function decodeHeader(buf: Buffer): Header {
  if (buf.length < HEADER_SIZE) {
    throw new Error(`Buffer underflow: expected ${HEADER_SIZE} bytes`);
  }
  const magic = buf.readUInt32LE(0);
  if (magic !== PROTOCOL_MAGIC) {
    throw new Error(`Invalid protocol magic: 0x${magic.toString(16)}`);
  }
  const version = buf.readUInt16LE(4);
  if (version !== PROTOCOL_VERSION) {
    throw new Error(`Unsupported protocol version: ${version}`);
  }
  const opcode = buf.readUInt16LE(6) as Opcode;
  const requestId = buf.readBigUInt64LE(8);
  const payloadLen = buf.readUInt32LE(16);
  return { magic, version, opcode, requestId, payloadLen };
}

// Encodes a command execution request payload.
export function encodeExecRequest(req: ExecRequest): Buffer {
  const commandBuf = Buffer.from(req.command, "utf-8");
  const cwdBuf = Buffer.from(req.cwd ?? "/tmp", "utf-8");
  let flags = 0;
  if (req.stripAnsi ?? true) flags |= EXEC_FLAG_STRIP_ANSI;
  if (req.captureDiff ?? true) flags |= EXEC_FLAG_CAPTURE_DIFF;
  const timeoutMs = req.timeoutMs ?? 30000;

  const header = Buffer.alloc(16);
  header.writeUInt32LE(flags, 0);
  header.writeUInt32LE(timeoutMs, 4);
  header.writeUInt32LE(commandBuf.length, 8);
  header.writeUInt32LE(cwdBuf.length, 12);
  return Buffer.concat([header, commandBuf, cwdBuf]);
}

// Decodes an execution response payload into an ExecResponse.
export function decodeExecResponse(buf: Buffer): ExecResponse {
  if (buf.length < 24) {
    throw new Error("Payload underflow decoding ExecResponse");
  }
  const exitCode = buf.readInt32LE(0);
  const flags = buf.readUInt32LE(4);
  const cpuCycles = buf.readBigUInt64LE(8);
  const outputLen = buf.readUInt32LE(16);
  const diffLen = buf.readUInt32LE(20);

  if (buf.length < 24 + outputLen + diffLen) {
    throw new Error("Payload underflow for output and diff segments");
  }
  const output = buf.subarray(24, 24 + outputLen).toString("utf-8");
  const diff = buf
    .subarray(24 + outputLen, 24 + outputLen + diffLen)
    .toString("utf-8");
  return { exitCode, flags, cpuCycles, output, diff };
}

// Encodes an AST symbol outline request payload.
export function encodeAstSymbolsRequest(filePath: string): Buffer {
  const pathBuf = Buffer.from(filePath, "utf-8");
  const hdr = Buffer.alloc(4);
  hdr.writeUInt32LE(pathBuf.length, 0);
  return Buffer.concat([hdr, pathBuf]);
}

// Decodes an AST symbol outline response payload.
export function decodeAstSymbolsResponse(buf: Buffer): AstSymbolsResponse {
  if (buf.length < 12) {
    throw new Error("Payload underflow decoding AstSymbolsResponse");
  }
  const status = buf.readInt32LE(0);
  const symbolCount = buf.readUInt32LE(4);
  const payloadLen = buf.readUInt32LE(8);
  if (buf.length < 12 + payloadLen) {
    throw new Error("Payload underflow for symbols text");
  }
  const symbolsText = buf.subarray(12, 12 + payloadLen).toString("utf-8");
  return { status, symbolCount, symbolsText };
}

// Encodes an AST symbol slice extraction request payload.
export function encodeAstSliceRequest(
  filePath: string,
  symbol: string
): Buffer {
  const pathBuf = Buffer.from(filePath, "utf-8");
  const symBuf = Buffer.from(symbol, "utf-8");
  const hdr = Buffer.alloc(4);
  hdr.writeUInt16LE(pathBuf.length, 0);
  hdr.writeUInt16LE(symBuf.length, 2);
  return Buffer.concat([hdr, pathBuf, symBuf]);
}

// Decodes an AST symbol slice response payload.
export function decodeAstSliceResponse(buf: Buffer): AstSliceResponse {
  if (buf.length < 16) {
    throw new Error("Payload underflow decoding AstSliceResponse");
  }
  const status = buf.readInt32LE(0);
  const startLine = buf.readUInt32LE(4);
  const endLine = buf.readUInt32LE(8);
  const contentLen = buf.readUInt32LE(12);
  if (buf.length < 16 + contentLen) {
    throw new Error("Payload underflow for slice content");
  }
  const content = buf.subarray(16, 16 + contentLen).toString("utf-8");
  return { status, startLine, endLine, content };
}

// Encodes an AST symbol replacement patch request payload.
export function encodeAstPatchRequest(
  filePath: string,
  symbol: string,
  replacement: string
): Buffer {
  const pathBuf = Buffer.from(filePath, "utf-8");
  const symBuf = Buffer.from(symbol, "utf-8");
  const repBuf = Buffer.from(replacement, "utf-8");
  const hdr = Buffer.alloc(8);
  hdr.writeUInt16LE(pathBuf.length, 0);
  hdr.writeUInt16LE(symBuf.length, 2);
  hdr.writeUInt32LE(repBuf.length, 4);
  return Buffer.concat([hdr, pathBuf, symBuf, repBuf]);
}

// Decodes an AST symbol replacement patch response payload.
export function decodeAstPatchResponse(buf: Buffer): AstPatchResponse {
  if (buf.length < 16) {
    throw new Error("Payload underflow decoding AstPatchResponse");
  }
  const status = buf.readInt32LE(0);
  const oldStartLine = buf.readUInt32LE(4);
  const oldEndLine = buf.readUInt32LE(8);
  const newEndLine = buf.readUInt32LE(12);
  return { status, oldStartLine, oldEndLine, newEndLine };
}

// Encodes a browser automation CDP action request payload.
export function encodeCdpRequest(req: CdpActionRequest): Buffer {
  const p1Buf = Buffer.from(req.payload1 ?? "", "utf-8");
  const p2Buf = Buffer.from(req.payload2 ?? "", "utf-8");
  const hdr = Buffer.alloc(20);
  hdr.writeUInt16LE(req.action, 0);
  hdr.writeUInt16LE(req.flags ?? 0, 2);
  hdr.writeUInt32LE(req.targetId ?? 0, 4);
  hdr.writeInt32LE(req.scrollDelta ?? 0, 8);
  hdr.writeUInt32LE(p1Buf.length, 12);
  hdr.writeUInt32LE(p2Buf.length, 16);
  return Buffer.concat([hdr, p1Buf, p2Buf]);
}

// Decodes a browser automation CDP action response payload.
export function decodeCdpResponse(buf: Buffer): CdpActionResponse {
  if (buf.length < 12) {
    throw new Error("Payload underflow decoding CdpActionResponse");
  }
  const status = buf.readInt32LE(0);
  const nodeCount = buf.readUInt32LE(4);
  const payloadLen = buf.readUInt32LE(8);
  if (buf.length < 12 + payloadLen) {
    throw new Error("Payload underflow for tree text");
  }
  const treeText = buf.subarray(12, 12 + payloadLen).toString("utf-8");
  return { status, nodeCount, treeText };
}
