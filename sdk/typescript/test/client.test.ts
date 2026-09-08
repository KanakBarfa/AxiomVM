// Integration tests for AxiomClient communication with appliance socket.

import test from "node:test";
import assert from "node:assert/strict";
import * as net from "net";
import { AxiomClient } from "../src/client.js";
import {
  HEADER_SIZE,
  Opcode,
  decodeHeader,
  encodeHeader,
} from "../src/wire.js";

test("AxiomClient client-server integration loopback", async () => {
  // Spawn in-memory wire server
  const server = net.createServer((socket) => {
    let accumulated = Buffer.alloc(0);

    socket.on("data", (chunk) => {
      accumulated = Buffer.concat([accumulated, chunk]);

      while (accumulated.length >= HEADER_SIZE) {
        const hdr = decodeHeader(accumulated.subarray(0, HEADER_SIZE));
        const total = HEADER_SIZE + hdr.payloadLen;
        if (accumulated.length < total) break;

        const payload = accumulated.subarray(HEADER_SIZE, total);
        accumulated = accumulated.subarray(total);

        if (hdr.opcode === Opcode.PING) {
          const respHdr = encodeHeader(Opcode.PONG, hdr.requestId, 0);
          socket.write(respHdr);
        } else if (hdr.opcode === Opcode.EXEC) {
          const out = Buffer.from("echo ok\n", "utf-8");
          const diff = Buffer.from("", "utf-8");
          const respPayload = Buffer.alloc(24 + out.length + diff.length);
          respPayload.writeInt32LE(0, 0);
          respPayload.writeUInt32LE(0, 4);
          respPayload.writeBigUInt64LE(50000n, 8);
          respPayload.writeUInt32LE(out.length, 16);
          respPayload.writeUInt32LE(diff.length, 20);
          out.copy(respPayload, 24);
          diff.copy(respPayload, 24 + out.length);

          const respHdr = encodeHeader(
            Opcode.EXEC_RESPONSE,
            hdr.requestId,
            respPayload.length
          );
          socket.write(Buffer.concat([respHdr, respPayload]));
        } else if (hdr.opcode === Opcode.AST_SYMBOLS) {
          const text = Buffer.from("[func] test\n", "utf-8");
          const respPayload = Buffer.alloc(12 + text.length);
          respPayload.writeInt32LE(0, 0);
          respPayload.writeUInt32LE(1, 4);
          respPayload.writeUInt32LE(text.length, 8);
          text.copy(respPayload, 12);

          const respHdr = encodeHeader(
            Opcode.AST_SYMBOLS_RESPONSE,
            hdr.requestId,
            respPayload.length
          );
          socket.write(Buffer.concat([respHdr, respPayload]));
        }
      }
    });
  });

  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  const addr = server.address() as net.AddressInfo;

  const client = new AxiomClient({ host: "127.0.0.1", port: addr.port });
  try {
    // 1. Ping
    const pingRes = await client.ping();
    assert.ok(pingRes.latencyMs >= 0);

    // 2. Exec
    const execRes = await client.exec("echo ok");
    assert.equal(execRes.exitCode, 0);
    assert.equal(execRes.output, "echo ok\n");
    assert.equal(execRes.cpuCycles, 50000n);

    // 3. Ast Symbols
    const symRes = await client.getSymbols("test.py");
    assert.equal(symRes.symbolCount, 1);
    assert.equal(symRes.symbolsText, "[func] test\n");
  } finally {
    await client.close();
    server.close();
  }
});
