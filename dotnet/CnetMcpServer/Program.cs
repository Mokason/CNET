using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using CnetMcpServer;

class Program
{
    static async Task Main(string[] args)
    {
        using var cts = new CancellationTokenSource();

        Console.CancelKeyPress += (sender, e) =>
        {
            e.Cancel = true;
            cts.Cancel();
            Console.Error.WriteLine("[CNET MCP] Shutdown signal received...");
        };

        Console.Error.WriteLine("[CNET MCP] Native host starting (stdio MCP mode)...");

        string basePath = Environment.GetEnvironmentVariable("CNET_BASE_PATH")
            ?? "/home/marble/AI/CNET/soul_gemma4v2_final.cnb";
        var tools = new CnetTools(basePath);

        // The toolGate serializes access to the native SoulHost / registry,
        // which is single-threaded state. Only operations that actually
        // touch SoulHost (VerifyClaim, RouteOnRole, ListUnits, ListOracles,
        // RequestCapability, HealthTick) need this lock. Compression
        // (CompressModel) works independently of SoulHost and must NOT
        // hold this lock — it has its own native path (CnetCompression)
        // and holding the authority lock during a long compression would
        // block all tool dispatch and the health timer.
        object toolGate = new object();

        // Opt-in periodic runtime health tick (specialist_health_pass over the
        // live registry). Disabled unless CNET_HEALTH_TICK_SECONDS > 0 — the
        // zero-init default changes nothing, matching the engine's house rule.
        int healthTickSeconds = int.TryParse(
            Environment.GetEnvironmentVariable("CNET_HEALTH_TICK_SECONDS"),
            out int hts) ? hts : 0;
        using var healthTimer = healthTickSeconds > 0
            ? new Timer(_ =>
            {
                try
                {
                    string tick;
                    lock (toolGate) { tick = tools.HealthTick(); }
                    Console.Error.WriteLine($"[CNET MCP] health tick: {tick}");
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine($"[CNET MCP] health tick failed: {ex.Message}");
                }
            }, null, TimeSpan.FromSeconds(healthTickSeconds),
               TimeSpan.FromSeconds(healthTickSeconds))
            : null;
        if (healthTickSeconds > 0)
            Console.Error.WriteLine($"[CNET MCP] periodic health tick every {healthTickSeconds}s");

        using var reader = new StreamReader(Console.OpenStandardInput());
        using var writer = new StreamWriter(Console.OpenStandardOutput()) { AutoFlush = true };
        using var writerGate = new SemaphoreSlim(1, 1);
        using var emptyArgsDocument = JsonDocument.Parse("{}");
        JsonElement emptyArgs = emptyArgsDocument.RootElement;
        var pendingOperations = new List<Task>();
        int compressionDelayMs = int.TryParse(
            Environment.GetEnvironmentVariable("CNET_MCP_TEST_COMPRESSION_DELAY_MS"),
            out int delayMs) && delayMs > 0 ? delayMs : 0;

        var jsonOpts = new JsonSerializerOptions
        {
            DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull
        };

        try
        {
            string? line;
            while (!cts.IsCancellationRequested && (line = await reader.ReadLineAsync()) != null)
            {
                if (string.IsNullOrWhiteSpace(line)) continue;
                await ObserveOperations(pendingOperations, waitForAll: false);

                JsonElement? responseId = null;
                bool responseExpected = false;
                try
                {
                // ---- Parse phase: -32700 on malformed JSON ----
                JsonElement request;
                try
                {
                    request = JsonSerializer.Deserialize<JsonElement>(line);
                }
                catch (JsonException)
                {
                    // Parse error: the input is not valid JSON.
                    // JSON-RPC 2.0: -32700 parse error. We have no id to
                    // echo back, so id is null.
                    await WriteError(writer, writerGate, null, -32700, "Parse error");
                    continue;
                }

                if (request.ValueKind != JsonValueKind.Object)
                {
                    await WriteError(writer, writerGate, null, -32600, "Invalid Request");
                    continue;
                }

                // ---- Extract id: preserve string, numeric, or null ----
                // If "id" is absent, this is a notification (JSON-RPC 2.0
                // says notifications must NOT receive a response).
                bool hasId = request.TryGetProperty("id", out var idElement);
                bool isNotification = !hasId;
                if (hasId)
                {
                    responseExpected = true;
                    responseId = idElement.Clone();
                }

                // ---- Extract method ----
                string method = "";
                if (request.TryGetProperty("method", out var mEl) &&
                    mEl.ValueKind == JsonValueKind.String)
                {
                    method = mEl.GetString() ?? "";
                }

                // ---- Validate JSON-RPC 2.0 envelope ----
                // A valid request has both "jsonrpc" and "method" fields.
                // A notification is a request without "id".
                // An invalid request (missing method, or not an object) gets -32600.
                bool hasJsonRpc = request.TryGetProperty("jsonrpc", out var jrEl) &&
                    jrEl.ValueKind == JsonValueKind.String &&
                    jrEl.GetString() == "2.0";
                bool validId = !hasId || idElement.ValueKind == JsonValueKind.String ||
                    idElement.ValueKind == JsonValueKind.Number ||
                    idElement.ValueKind == JsonValueKind.Null;

                if (!hasJsonRpc || !validId || string.IsNullOrEmpty(method))
                {
                    // Invalid notifications remain silent. Requests preserve a
                    // valid id; malformed id values are represented as null.
                    if (!isNotification)
                        await WriteError(writer, writerGate, validId ? idElement : (JsonElement?)null,
                            -32600, "Invalid Request");
                    continue;
                }

                // ---- Dispatch ----
                if (method == "initialize")
                {
                    string protocolVersion = "2024-11-05";
                    if (request.TryGetProperty("params", out var initParams))
                    {
                        if (initParams.ValueKind != JsonValueKind.Object)
                        {
                            if (!isNotification)
                                await WriteError(writer, writerGate, idElement, -32602,
                                    "Invalid params: expected object");
                            continue;
                        }
                        if (initParams.TryGetProperty("protocolVersion", out var pv) &&
                            pv.ValueKind == JsonValueKind.String)
                            protocolVersion = pv.GetString() ?? protocolVersion;
                    }
                    var response = new
                    {
                        jsonrpc = "2.0",
                        id = hasId ? (object?)GetIdValue(idElement) : null,
                        result = new
                        {
                            protocolVersion,
                            capabilities = new { tools = new { } },
                            serverInfo = new { name = "cnet-mcp", version = "0.2.0" }
                        }
                    };
                    if (!isNotification)
                        await WriteFrame(writer, writerGate,
                            JsonSerializer.Serialize(response, jsonOpts));
                }
                else if (method == "tools/list")
                {
                    var response = new
                    {
                        jsonrpc = "2.0",
                        id = hasId ? (object?)GetIdValue(idElement) : null,
                        result = new
                        {
                            tools = new object[]
                            {
                                new
                                {
                                    name = "cnet_verify_claim",
                                    description = "Verify a claim against CNET units with optional counterfactual route evidence",
                                    inputSchema = (object)new
                                    {
                                        type = "object",
                                        properties = new
                                        {
                                            claim = new { type = "string", description = "The claim to verify" },
                                            unitTag = new { type = "string", description = "Optional CNET unit tag to verify against" },
                                            codebaseNodes = new { type = "array", items = new { type = "string" }, description = "Optional codebase node references" },
                                            counterfactualRoutes = new { type = "array", items = new { type = "string" }, description = "Optional counterfactual route evidence" },
                                            counterfactualConsistency = new { type = "number", description = "Optional consistency score for counterfactual routes" }
                                        },
                                        required = new[] { "claim" }
                                    }
                                },
                                new
                                {
                                    name = "cnet_generate_testimony",
                                    description = "Generate Memory-Witness testimony with narrative coherence scoring (place + dilemma + consequence)",
                                    inputSchema = (object)new
                                    {
                                        type = "object",
                                        properties = new
                                        {
                                            place = new { type = "string", description = "Where the events took place" },
                                            dilemma = new { type = "string", description = "The dilemma faced" },
                                            consequence = new { type = "string", description = "The consequence that followed" },
                                            codebaseNodes = new { type = "array", items = new { type = "string" }, description = "Optional codebase node references" }
                                        },
                                        required = new[] { "place", "dilemma", "consequence" }
                                    }
                                },
                                new
                                {
                                    name = "cnet_compress_model",
                                    description = "Prepare a CNET-compressed model wrapper for Hermes hosting",
                                    inputSchema = (object)new
                                    {
                                        type = "object",
                                        properties = new
                                        {
                                            model_path = new { type = "string", description = "Path to the model to compress" },
                                            target_size = new { type = "string", description = "Target compression size (default 1.6bit)" },
                                            options = new { type = "string", description = "Extra compression options" }
                                        },
                                        required = new[] { "model_path" }
                                    }
                                },
                                new
                                {
                                    name = "cnet_expand_context",
                                    description = "Expand context with AICIMO routing + uncertainty",
                                    inputSchema = (object)new
                                    {
                                        type = "object",
                                        properties = new
                                        {
                                            input = new { type = "string", description = "Input text to expand" },
                                            baseDim = new { type = "integer", description = "Base dimension (default 8192)" }
                                        },
                                        required = new[] { "input" }
                                    }
                                },
                                new
                                {
                                    name = "cnet_route_on_role",
                                    description = "Route using AICIMO role-slice (Drole)",
                                    inputSchema = (object)new
                                    {
                                        type = "object",
                                        properties = new
                                        {
                                            input = new { type = "string", description = "Input text to route" },
                                            role = new { type = "string", description = "Role slice (default memory-witness)" }
                                        },
                                        required = new[] { "input" }
                                    }
                                },
                                new
                                {
                                    name = "cnet_list_units",
                                    description = "List available CNET units",
                                    inputSchema = (object)new { type = "object", properties = new { } }
                                },
                                new
                                {
                                    name = "cnet_list_oracles",
                                    description = "List bounded Oracle provenance from the authoritative native base",
                                    inputSchema = (object)new { type = "object", properties = new { } }
                                },
                                new
                                {
                                    name = "cnet_request_capability",
                                    description = "Request a capability by explicit typed signature: served now if a certified plan exists, else the novel goal is queued to the gap inbox for the 24/7 gap lane to acquire from the local model",
                                    inputSchema = (object)new
                                    {
                                        type = "object",
                                        properties = new
                                        {
                                            goal_tag = new { type = "string", description = "Semantic tag of the requested goal port" },
                                            in_tag = new { type = "string", description = "Semantic tag of the input port (REQUIRED: a wildcard input is unservable; the deployed soul's convention is w_cur)" },
                                            family = new { type = "string", description = "Port family: onehot|binary_msb|binary_lsb|raw (default onehot)" },
                                            width = new { type = "integer", description = "Field width (default 256)" },
                                            count = new { type = "integer", description = "Input field count (default 1)" },
                                            goal_count = new { type = "integer", description = "Goal field count, e.g. top-k (default 1)" },
                                            input = new { type = "array", items = new { type = "number" }, description = "Optional input vector; omit for a capability probe" }
                                        },
                                        required = new[] { "goal_tag", "in_tag" }
                                    }
                                },
                                new
                                {
                                    name = "cnet_health_tick",
                                    description = "Run one runtime health pass (audit, fault labeling, heal via re-certify, evidence promotion, shadow swap) over the live certified registry and report exact counts",
                                    inputSchema = (object)new { type = "object", properties = new { } }
                                },
                                new
                                {
                                    name = "cnet_classify_toolcall",
                                    description = "Classify agent JSON into a closed-set tool via sealed json_toolcall_v1 (certified; includes web_search/wiki_lookup). On miss notes gap jtc_feat→json_tool for the personal-AI lane.",
                                    inputSchema = (object)new
                                    {
                                        type = "object",
                                        properties = new
                                        {
                                            json = new { type = "string", description = "Agent tool-call JSON object, e.g. {\"tool\":\"web_search\",\"args\":{\"query\":\"...\"}}" }
                                        },
                                        required = new[] { "json" }
                                    }
                                },
                                new
                                {
                                    name = "cnet_json_toolcall_status",
                                    description = "Report whether json_toolcall unit is sealed in the live base and sample-classify calculator example",
                                    inputSchema = (object)new { type = "object", properties = new { } }
                                },
                                new
                                {
                                    name = "cnet_web_search",
                                    description = "Look up facts on the web (DuckDuckGo Instant Answer; falls back to Wikipedia). Use when the agent needs external knowledge.",
                                    inputSchema = (object)new
                                    {
                                        type = "object",
                                        properties = new
                                        {
                                            query = new { type = "string", description = "Search terms / question" }
                                        },
                                        required = new[] { "query" }
                                    }
                                },
                                new
                                {
                                    name = "cnet_wiki_lookup",
                                    description = "Look up a Wikipedia summary for a person, place, or topic. Memory-cached.",
                                    inputSchema = (object)new
                                    {
                                        type = "object",
                                        properties = new
                                        {
                                            query = new { type = "string", description = "Entity or topic name" }
                                        },
                                        required = new[] { "query" }
                                    }
                                }
                            }
                        }
                    };
                    if (!isNotification)
                        await WriteFrame(writer, writerGate,
                            JsonSerializer.Serialize(response, jsonOpts));
                }
                else if (method == "tools/call")
                {
                    // MCP tool-call params must be a JSON object.
                    if (!request.TryGetProperty("params", out var callParams) ||
                        callParams.ValueKind != JsonValueKind.Object)
                    {
                        if (!isNotification)
                            await WriteError(writer, writerGate, hasId ? idElement : (JsonElement?)null,
                                -32602, "Invalid params: missing params object");
                        continue;
                    }

                    // Validate params.name (tool name) presence
                    if (!callParams.TryGetProperty("name", out var nameEl) ||
                        nameEl.ValueKind != JsonValueKind.String)
                    {
                        if (!isNotification)
                            await WriteError(writer, writerGate, hasId ? idElement : (JsonElement?)null,
                                -32602, "Invalid params: missing or invalid tool name");
                        continue;
                    }

                    var toolName = nameEl.GetString() ?? "";
                    // arguments is optional — default to empty object
                    var toolArgs = callParams.TryGetProperty("arguments", out var argsEl)
                        ? argsEl : emptyArgs;
                    if (toolArgs.ValueKind != JsonValueKind.Object)
                    {
                        if (!isNotification)
                            await WriteError(writer, writerGate, idElement, -32602,
                                "Invalid params: arguments must be an object");
                        continue;
                    }

                    if (toolName == "cnet_compress_model")
                    {
                        JsonElement capturedArgs = toolArgs.ValueKind == JsonValueKind.Object
                            ? toolArgs.Clone() : default;
                        JsonElement capturedId = hasId ? idElement.Clone() : default;
                        bool capturedNotification = isNotification;
                        pendingOperations.Add(Task.Run(async () =>
                        {
                            string resultText;
                            try
                            {
                                if (compressionDelayMs > 0)
                                    await Task.Delay(compressionDelayMs);
                                string targetSize = SafeGetString(capturedArgs, "target_size");
                                resultText = tools.CompressModel(
                                    SafeGetString(capturedArgs, "model_path"),
                                    string.IsNullOrEmpty(targetSize) ? "1.6bit" : targetSize,
                                    SafeGetString(capturedArgs, "options"));
                            }
                            catch (Exception toolExc)
                            {
                                Console.Error.WriteLine(
                                    $"[CNET MCP] tool '{toolName}' failed: {toolExc}");
                                resultText =
                                    $"[CNET] Tool '{toolName}' failed: {toolExc.Message}";
                            }
                            if (!capturedNotification)
                            {
                                var response = new
                                {
                                    jsonrpc = "2.0",
                                    id = (object?)GetIdValue(capturedId),
                                    result = new
                                    {
                                        content = new[]
                                        {
                                            new { type = "text", text = resultText }
                                        }
                                    }
                                };
                                await WriteFrame(writer, writerGate,
                                    JsonSerializer.Serialize(response, jsonOpts));
                            }
                        }));
                        continue;
                    }

                    // A tool failure must never kill the stdio server: report
                    // it as the tool result instead of unwinding the read loop.
                    string resultText;
                    try
                    {
                        lock (toolGate)
                        {
                            resultText = toolName switch
                            {
                                    "cnet_verify_claim" => tools.VerifyClaim(
                                        SafeGetString(toolArgs, "claim"),
                                        toolArgs.TryGetProperty("unitTag", out var ut) ? ut.GetString() ?? "" : "",
                                        ReadStringList(toolArgs, "codebaseNodes"),
                                        ReadStringList(toolArgs, "counterfactualRoutes"),
                                        ReadOptionalDouble(toolArgs, "counterfactualConsistency")),
                                    "cnet_generate_testimony" => tools.GenerateTestimony(
                                        SafeGetString(toolArgs, "place"),
                                        SafeGetString(toolArgs, "dilemma"),
                                        SafeGetString(toolArgs, "consequence"),
                                        ReadStringList(toolArgs, "codebaseNodes")),
                                    "cnet_expand_context" => tools.ExpandContext(
                                        SafeGetString(toolArgs, "input"),
                                        toolArgs.TryGetProperty("baseDim", out var bd) && bd.ValueKind == JsonValueKind.Number ? bd.GetInt32() : 8192),
                                    "cnet_route_on_role" => tools.RouteOnRole(
                                        SafeGetString(toolArgs, "input"),
                                        toolArgs.TryGetProperty("role", out var r) ? r.GetString() ?? "memory-witness" : "memory-witness"),
                                    "cnet_list_units" => tools.ListUnits(),
                                    "cnet_list_oracles" => tools.ListOracles(),
                                    "cnet_request_capability" => tools.RequestCapability(
                                        SafeGetString(toolArgs, "goal_tag"),
                                        toolArgs.TryGetProperty("in_tag", out var rit) ? rit.GetString() ?? "" : "",
                                        toolArgs.TryGetProperty("family", out var rf) ? rf.GetString() ?? "onehot" : "onehot",
                                        toolArgs.TryGetProperty("width", out var rw) && rw.ValueKind == JsonValueKind.Number ? rw.GetInt32() : 256,
                                        toolArgs.TryGetProperty("count", out var rcnt) && rcnt.ValueKind == JsonValueKind.Number ? rcnt.GetInt32() : 1,
                                        toolArgs.TryGetProperty("goal_count", out var rgc) && rgc.ValueKind == JsonValueKind.Number ? rgc.GetInt32() : 1,
                                        ReadDoubleList(toolArgs, "input")),
                                    "cnet_health_tick" => tools.HealthTick(),
                                    "cnet_classify_toolcall" => tools.ClassifyToolCall(
                                        SafeGetString(toolArgs, "json")),
                                    "cnet_json_toolcall_status" => tools.JsonToolCallStatus(),
                                    "cnet_web_search" => tools.WebSearch(
                                        SafeGetString(toolArgs, "query")),
                                    "cnet_wiki_lookup" => tools.WikiLookup(
                                        SafeGetString(toolArgs, "query")),
                                    _ => "Unknown tool: " + toolName
                            };
                        }
                    }
                    catch (Exception toolExc)
                    {
                        Console.Error.WriteLine($"[CNET MCP] tool '{toolName}' failed: {toolExc}");
                        resultText = $"[CNET] Tool '{toolName}' failed: {toolExc.Message}";
                    }

                    if (!isNotification)
                    {
                        var response = new
                        {
                            jsonrpc = "2.0",
                            id = (object?)GetIdValue(idElement),
                            result = new { content = new[] { new { type = "text", text = resultText } } }
                        };
                        await WriteFrame(writer, writerGate,
                            JsonSerializer.Serialize(response, jsonOpts));
                    }
                }
                else if (method == "initialized" || method == "notifications/initialized")
                {
                    // Standard MCP notification — no response (JSON-RPC silence)
                }
                else
                {
                    // Unknown method → -32601 method not found
                    if (!isNotification)
                        await WriteError(writer, writerGate, hasId ? idElement : (JsonElement?)null,
                            -32601, $"Method not found: {method}");
                }
                }
                catch (Exception frameException)
                {
                    Console.Error.WriteLine(
                        $"[CNET MCP] request frame failed: {frameException}");
                    if (responseExpected)
                        await WriteError(writer, writerGate, responseId,
                            -32603, "Internal error");
                }
            }
            await ObserveOperations(pendingOperations, waitForAll: true);
        }
        catch (OperationCanceledException)
        {
            Console.Error.WriteLine("[CNET MCP] Graceful shutdown complete.");
        }
    }

    static async Task ObserveOperations(List<Task> operations, bool waitForAll)
    {
        for (int index = operations.Count - 1; index >= 0; index--)
        {
            Task operation = operations[index];
            if (!waitForAll && !operation.IsCompleted) continue;
            try
            {
                await operation;
            }
            catch (Exception operationException)
            {
                Console.Error.WriteLine(
                    $"[CNET MCP] independent operation failed: {operationException}");
            }
            operations.RemoveAt(index);
        }
    }

    /// <summary>
    /// Write a JSON-RPC 2.0 error response. The id is preserved as-is
    /// (string, number, or null) by serializing the raw JsonElement.
    /// </summary>
    static async Task WriteFrame(StreamWriter writer, SemaphoreSlim writerGate,
                                 string frame)
    {
        await writerGate.WaitAsync();
        try
        {
            await writer.WriteLineAsync(frame);
        }
        finally
        {
            writerGate.Release();
        }
    }

    static async Task WriteError(StreamWriter writer, SemaphoreSlim writerGate,
                                 JsonElement? id, int code, string message)
    {
        // Build the error response manually to preserve id type fidelity.
        // We serialize the id element directly to preserve its JSON type
        // (string, number, null, etc.).
        string idJson = id.HasValue ? id.Value.GetRawText() : "null";

        string errorJson = JsonSerializer.Serialize(new
        {
            code,
            message
        });

        string frame = $"{{\"jsonrpc\":\"2.0\",\"id\":{idJson},\"error\":{errorJson}}}";
        await WriteFrame(writer, writerGate, frame);
    }

    /// <summary>
    /// Extract the id value preserving its JSON type. Returns object? that
    /// JsonSerializer will serialize with correct type (string, int, long, etc.)
    /// </summary>
    static object? GetIdValue(JsonElement idElement)
    {
        return idElement.ValueKind switch
        {
            JsonValueKind.String => idElement.GetString(),
            JsonValueKind.Number when idElement.TryGetInt64(out long l) => l,
            JsonValueKind.Number when idElement.TryGetDouble(out double d) => d,
            JsonValueKind.Null => null,
            _ => null
        };
    }

    /// <summary>
    /// Safely get a string property from a JsonElement, returning "" if
    /// the property is missing or not a string. Never throws.
    /// </summary>
    static string SafeGetString(JsonElement element, string propertyName)
    {
        if (element.ValueKind != JsonValueKind.Object) return "";
        if (!element.TryGetProperty(propertyName, out var prop)) return "";
        if (prop.ValueKind != JsonValueKind.String) return "";
        return prop.GetString() ?? "";
    }

    /// <summary>
    /// Overload for default(JsonElement) — handles the case where arguments
    /// was absent and we're working with a default-valued JsonElement.
    /// </summary>
    static string SafeGetString(string propertyName, JsonElement element)
    {
        return SafeGetString(element, propertyName);
    }

    private static List<string> ReadStringList(JsonElement args, string propertyName)
    {
        var values = new List<string>();
        if (args.ValueKind != JsonValueKind.Object) return values;
        if (!args.TryGetProperty(propertyName, out var element) ||
            element.ValueKind != JsonValueKind.Array)
        {
            return values;
        }

        foreach (var item in element.EnumerateArray())
        {
            if (item.ValueKind == JsonValueKind.String)
            {
                values.Add(item.GetString() ?? "");
            }
        }
        return values;
    }

    private static List<double> ReadDoubleList(JsonElement args, string propertyName)
    {
        var values = new List<double>();
        if (args.ValueKind != JsonValueKind.Object) return values;
        if (!args.TryGetProperty(propertyName, out var element) ||
            element.ValueKind != JsonValueKind.Array)
        {
            return values;
        }
        foreach (var item in element.EnumerateArray())
        {
            if (item.ValueKind == JsonValueKind.Number)
            {
                values.Add(item.GetDouble());
            }
        }
        return values;
    }

    private static double? ReadOptionalDouble(JsonElement args, string propertyName)
    {
        if (args.ValueKind != JsonValueKind.Object) return null;
        if (!args.TryGetProperty(propertyName, out var element) ||
            element.ValueKind != JsonValueKind.Number)
        {
            return null;
        }
        return element.GetDouble();
    }
}