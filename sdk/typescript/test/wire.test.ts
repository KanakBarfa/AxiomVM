// Integration tests for wire protocol frame encoding and decoding.

import test from "node:test";
import assert from "node:assert/strict";
import {
  CdpActionType,
  HEADER_SIZE,
  Opcode,
  WIRE_MAGIC,
  decodeAstPatchResponse,
  decodeAstSliceResponse,
  decodeAstSymbolsResponse,
  decodeCdpResponse,
  decodeExecResponse,
  decodeHeader,
  decodeReadFileResponse,
  decodeWriteFileResponse,
  encodeAstPatchRequest,
  encodeAstSliceRequest,
  encodeAstSymbolsRequest,
  encodeCdpRequest,
  encodeExecRequest,
  encodeHeader,
  encodeReadFileRequest,
  encodeWriteFileRequest,
} from "../src/wire.js";

test("wire header encode and decode roundtrip", () => {
  const buf = encodeHeader(Opcode.PING, 42, 128);
  assert.equal(buf.length, HEADER_SIZE);
  assert.equal(buf.length, 12);
  const hdr = decodeHeader(buf);
  assert.equal(hdr.magic, WIRE_MAGIC);
  assert.equal(hdr.opcode, Opcode.PING);
  assert.equal(hdr.requestId, 42);
  assert.equal(hdr.payloadLen, 128);
});

test("exec request and response roundtrip", () => {
  const reqBuf = encodeExecRequest({
    command: "ls -la",
    cwd: "/home",
    stripAnsi: true,
    captureDiff: true,
    timeoutMs: 5000,
  });
  assert.ok(reqBuf.length >= 12);

  const payload = Buffer.alloc(24 + 5 + 4);
  payload.writeInt32LE(0, 0);
  payload.writeUInt32LE(3, 4);
  payload.writeBigUInt64LE(1000n, 8);
  payload.writeUInt32LE(5, 16);
  payload.writeUInt32LE(4, 20);
  payload.write("hello", 24, "utf-8");
  payload.write("diff", 29, "utf-8");

  const resp = decodeExecResponse(payload);
  assert.equal(resp.exitCode, 0);
  assert.equal(resp.flags, 3);
  assert.equal(resp.cpuCycles, 1000n);
  assert.equal(resp.output, "hello");
  assert.equal(resp.diff, "diff");
});

test("ast symbols request and response roundtrip", () => {
  const reqBuf = encodeAstSymbolsRequest("foo.py");
  assert.ok(reqBuf.length > 4);

  const textBuf = Buffer.from("[function] foo [L1-L5]\n", "utf-8");
  const payload = Buffer.alloc(12 + textBuf.length);
  payload.writeInt32LE(0, 0);
  payload.writeUInt32LE(1, 4);
  payload.writeUInt32LE(textBuf.length, 8);
  textBuf.copy(payload, 12);

  const resp = decodeAstSymbolsResponse(payload);
  assert.equal(resp.status, 0);
  assert.equal(resp.symbolCount, 1);
  assert.equal(resp.symbolsText, "[function] foo [L1-L5]\n");
});

test("ast slice request and response roundtrip", () => {
  const reqBuf = encodeAstSliceRequest("foo.py", "foo");
  assert.ok(reqBuf.length > 4);

  const codeBuf = Buffer.from("def foo():\n    pass\n", "utf-8");
  const payload = Buffer.alloc(16 + codeBuf.length);
  payload.writeInt32LE(0, 0);
  payload.writeUInt32LE(1, 4);
  payload.writeUInt32LE(2, 8);
  payload.writeUInt32LE(codeBuf.length, 12);
  codeBuf.copy(payload, 16);

  const resp = decodeAstSliceResponse(payload);
  assert.equal(resp.status, 0);
  assert.equal(resp.startLine, 1);
  assert.equal(resp.endLine, 2);
  assert.equal(resp.content, "def foo():\n    pass\n");
});

test("ast patch request and response roundtrip", () => {
  const reqBuf = encodeAstPatchRequest("foo.py", "foo", "def foo(): return 1");
  assert.ok(reqBuf.length > 8);

  const payload = Buffer.alloc(16);
  payload.writeInt32LE(0, 0);
  payload.writeUInt32LE(1, 4);
  payload.writeUInt32LE(2, 8);
  payload.writeUInt32LE(1, 12);

  const resp = decodeAstPatchResponse(payload);
  assert.equal(resp.status, 0);
  assert.equal(resp.oldStartLine, 1);
  assert.equal(resp.oldEndLine, 2);
  assert.equal(resp.newEndLine, 1);
});

test("cdp action request and response roundtrip", () => {
  const reqBuf = encodeCdpRequest({
    action: CdpActionType.NAVIGATE,
    payload1: "http://127.0.0.1",
  });
  assert.ok(reqBuf.length > 20);

  const treeBuf = Buffer.from('[1] RootWebArea "Index"', "utf-8");
  const payload = Buffer.alloc(12 + treeBuf.length);
  payload.writeInt32LE(0, 0);
  payload.writeUInt32LE(1, 4);
  payload.writeUInt32LE(treeBuf.length, 8);
  treeBuf.copy(payload, 12);

  const resp = decodeCdpResponse(payload);
  assert.equal(resp.status, 0);
  assert.equal(resp.nodeCount, 1);
  assert.equal(resp.treeText, '[1] RootWebArea "Index"');
});

test("file read and write request/response roundtrip", () => {
  const writeReq = encodeWriteFileRequest("/tmp/test.txt", "content");
  assert.ok(writeReq.length >= 12);

  const writeRespBuf = Buffer.alloc(8);
  writeRespBuf.writeInt32LE(0, 0);
  writeRespBuf.writeUInt32LE(7, 4);
  const writeResp = decodeWriteFileResponse(writeRespBuf);
  assert.equal(writeResp.status, 0);
  assert.equal(writeResp.bytesWritten, 7);

  const readReq = encodeReadFileRequest("/tmp/test.txt");
  assert.ok(readReq.length >= 4);

  const readData = Buffer.from("content", "utf-8");
  const readRespBuf = Buffer.alloc(12 + readData.length);
  readRespBuf.writeInt32LE(0, 0);
  readRespBuf.writeUInt32LE(readData.length, 4);
  readRespBuf.writeUInt32LE(readData.length, 8);
  readData.copy(readRespBuf, 12);

  const readResp = decodeReadFileResponse(readRespBuf);
  assert.equal(readResp.status, 0);
  assert.equal(readResp.totalSize, 7);
  assert.equal(readResp.content.toString("utf-8"), "content");
});
