// High-level typed client for interacting with the AxiomVM appliance.

import * as net from "net";
import {
  AstPatchResponse,
  AstSliceResponse,
  AstSymbolsResponse,
  CdpActionResponse,
  CdpActionType,
  ExecRequest,
  ExecResponse,
  HEADER_SIZE,
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
} from "./wire.js";

export interface AxiomClientOptions {
  socketPath?: string;
  host?: string;
  port?: number;
  timeoutMs?: number;
}

// Client for sending execution, AST, and browser commands to AxiomVM.
export class AxiomClient {
  private socketPath?: string;
  private host?: string;
  private port?: number;
  private timeoutMs: number;
  private socket: net.Socket | null = null;
  private requestIdCounter: bigint = 1n;
  private buffer: Buffer = Buffer.alloc(0);

  constructor(options: AxiomClientOptions = {}) {
    this.socketPath = options.socketPath;
    this.host = options.host;
    this.port = options.port;
    this.timeoutMs = options.timeoutMs ?? 10000;
  }

  // Establishes a connection to the microVM bridge socket.
  async connect(): Promise<void> {
    if (this.socket && !this.socket.destroyed) {
      return;
    }

    return new Promise((resolve, reject) => {
      const connHandler = () => {
        cleanup();
        resolve();
      };
      const errorHandler = (err: Error) => {
        cleanup();
        reject(err);
      };

      let sock: net.Socket;
      if (this.socketPath) {
        sock = net.createConnection(this.socketPath, connHandler);
      } else if (this.host && this.port) {
        sock = net.createConnection(this.port, this.host, connHandler);
      } else {
        reject(new Error("Either socketPath or host and port must be specified"));
        return;
      }

      const cleanup = () => {
        sock.removeListener("connect", connHandler);
        sock.removeListener("error", errorHandler);
      };

      sock.once("error", errorHandler);
      sock.on("data", (chunk: Buffer) => {
        this.buffer = Buffer.concat([this.buffer, chunk]);
      });

      this.socket = sock;
    });
  }

  // Closes the connection to the microVM bridge.
  async close(): Promise<void> {
    if (this.socket) {
      this.socket.destroy();
      this.socket = null;
      this.buffer = Buffer.alloc(0);
    }
  }

  // Internal helper to exchange a framed binary request and await response.
  private async exchange(
    opcode: Opcode,
    payload: Buffer,
    expectedResponseOpcode: Opcode
  ): Promise<{ payload: Buffer; latencyMs: number }> {
    await this.connect();
    if (!this.socket) {
      throw new Error("Socket disconnected");
    }

    const reqId = this.requestIdCounter++;
    const header = encodeHeader(opcode, reqId, payload.length);
    const frame = Buffer.concat([header, payload]);

    const startTime = performance.now();
    this.socket.write(frame);

    // Wait until full header arrives
    while (this.buffer.length < HEADER_SIZE) {
      await new Promise((resolve) => setTimeout(resolve, 1));
      if (performance.now() - startTime > this.timeoutMs) {
        throw new Error("Timeout waiting for response header");
      }
    }

    const respHeader = decodeHeader(this.buffer.subarray(0, HEADER_SIZE));
    if (respHeader.opcode !== expectedResponseOpcode) {
      throw new Error(
        `Unexpected opcode: 0x${respHeader.opcode.toString(16)}, expected 0x${expectedResponseOpcode.toString(16)}`
      );
    }

    const totalExpected = HEADER_SIZE + respHeader.payloadLen;
    while (this.buffer.length < totalExpected) {
      await new Promise((resolve) => setTimeout(resolve, 1));
      if (performance.now() - startTime > this.timeoutMs) {
        throw new Error("Timeout waiting for response payload");
      }
    }

    const respPayload = this.buffer.subarray(HEADER_SIZE, totalExpected);
    this.buffer = this.buffer.subarray(totalExpected);
    const latencyMs = performance.now() - startTime;

    return { payload: respPayload, latencyMs };
  }

  // Sends a ping heartbeat to verify appliance liveness.
  async ping(): Promise<{ latencyMs: number }> {
    const { latencyMs } = await this.exchange(
      Opcode.PING,
      Buffer.alloc(0),
      Opcode.PONG
    );
    return { latencyMs };
  }

  // Executes a command inside the microVM sandbox.
  async exec(
    command: string,
    options: Omit<ExecRequest, "command"> = {}
  ): Promise<ExecResponse & { latencyMs: number }> {
    const req: ExecRequest = { command, ...options };
    const payload = encodeExecRequest(req);
    const { payload: respPayload, latencyMs } = await this.exchange(
      Opcode.EXEC,
      payload,
      Opcode.EXEC_RESPONSE
    );
    const resp = decodeExecResponse(respPayload);
    return { ...resp, latencyMs };
  }

  // Queries AST symbol outlines for a file.
  async getSymbols(
    filePath: string
  ): Promise<AstSymbolsResponse & { latencyMs: number }> {
    const payload = encodeAstSymbolsRequest(filePath);
    const { payload: respPayload, latencyMs } = await this.exchange(
      Opcode.AST_SYMBOLS,
      payload,
      Opcode.AST_SYMBOLS_RESPONSE
    );
    const resp = decodeAstSymbolsResponse(respPayload);
    return { ...resp, latencyMs };
  }

  // Extracts source lines for a target symbol.
  async getSlice(
    filePath: string,
    symbol: string
  ): Promise<AstSliceResponse & { latencyMs: number }> {
    const payload = encodeAstSliceRequest(filePath, symbol);
    const { payload: respPayload, latencyMs } = await this.exchange(
      Opcode.AST_SLICE,
      payload,
      Opcode.AST_SLICE_RESPONSE
    );
    const resp = decodeAstSliceResponse(respPayload);
    return { ...resp, latencyMs };
  }

  // Replaces a symbol implementation atomically.
  async patchSymbol(
    filePath: string,
    symbol: string,
    replacement: string
  ): Promise<AstPatchResponse & { latencyMs: number }> {
    const payload = encodeAstPatchRequest(filePath, symbol, replacement);
    const { payload: respPayload, latencyMs } = await this.exchange(
      Opcode.AST_PATCH,
      payload,
      Opcode.AST_PATCH_RESPONSE
    );
    const resp = decodeAstPatchResponse(respPayload);
    return { ...resp, latencyMs };
  }

  // Navigates headless Chromium to a URL and returns semantic accessibility tree.
  async navigate(
    url: string
  ): Promise<CdpActionResponse & { latencyMs: number }> {
    const payload = encodeCdpRequest({
      action: CdpActionType.NAVIGATE,
      payload1: url,
    });
    const { payload: respPayload, latencyMs } = await this.exchange(
      Opcode.CDP_ACTION,
      payload,
      Opcode.CDP_ACTION_RESPONSE
    );
    const resp = decodeCdpResponse(respPayload);
    return { ...resp, latencyMs };
  }

  // Clicks an element by semantic tree target ID.
  async click(
    targetId: number
  ): Promise<CdpActionResponse & { latencyMs: number }> {
    const payload = encodeCdpRequest({
      action: CdpActionType.CLICK,
      targetId,
    });
    const { payload: respPayload, latencyMs } = await this.exchange(
      Opcode.CDP_ACTION,
      payload,
      Opcode.CDP_ACTION_RESPONSE
    );
    const resp = decodeCdpResponse(respPayload);
    return { ...resp, latencyMs };
  }

  // Types text into an element by semantic target ID.
  async typeText(
    targetId: number,
    text: string
  ): Promise<CdpActionResponse & { latencyMs: number }> {
    const payload = encodeCdpRequest({
      action: CdpActionType.TYPE_TEXT,
      targetId,
      payload1: text,
    });
    const { payload: respPayload, latencyMs } = await this.exchange(
      Opcode.CDP_ACTION,
      payload,
      Opcode.CDP_ACTION_RESPONSE
    );
    const resp = decodeCdpResponse(respPayload);
    return { ...resp, latencyMs };
  }

  // Scrolls viewport or container by delta pixels.
  async scroll(
    delta: number
  ): Promise<CdpActionResponse & { latencyMs: number }> {
    const payload = encodeCdpRequest({
      action: CdpActionType.SCROLL,
      scrollDelta: delta,
    });
    const { payload: respPayload, latencyMs } = await this.exchange(
      Opcode.CDP_ACTION,
      payload,
      Opcode.CDP_ACTION_RESPONSE
    );
    const resp = decodeCdpResponse(respPayload);
    return { ...resp, latencyMs };
  }

  // Retrieves current accessibility tree from headless Chromium.
  async getAxTree(): Promise<CdpActionResponse & { latencyMs: number }> {
    const payload = encodeCdpRequest({
      action: CdpActionType.GET_TREE,
    });
    const { payload: respPayload, latencyMs } = await this.exchange(
      Opcode.CDP_ACTION,
      payload,
      Opcode.CDP_ACTION_RESPONSE
    );
    const resp = decodeCdpResponse(respPayload);
    return { ...resp, latencyMs };
  }
}
