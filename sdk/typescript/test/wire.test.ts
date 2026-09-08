// Integration tests for wire protocol frame encoding and decoding.

import test from "node:test";
import assert from "node:assert/strict";
import {
  Opcode,
  decodeAstPatchResponse,
  decodeAstSliceResponse,
  decodeAstSymbolsResponse,
  decodeCdpResponse,
  decodeExecResponse,
  decodeHeader,
  encodeAstPatchRequest,
  encodeAstSliceRequest,
  encodeAstSymbolsRequest,
  encodeCdpRequest,
  encodeExecRequest,
  encodeHeader,
  CdpActionType,
} from "../src/wire.js";

test("wire header encode and decode roundtrip", () => {
  const buf = encodeHeader(Opcode.PING, 42n, 128);
  const hdr = decodeHeader(buf);
  assert.equal(hdr.opcode, Opcode.PING);
  assert.equal(hdr.requestId, 42n);
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
  assert.ok(reqBuf.length >= 16);

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
