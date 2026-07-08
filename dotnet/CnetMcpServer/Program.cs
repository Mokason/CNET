using System;
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

        var tools = new CnetTools("/home/marble/AI/CNET/soul_gemma4v2_final.cnb");

        using var reader = new StreamReader(Console.OpenStandardInput());
        using var writer = new StreamWriter(Console.OpenStandardOutput()) { AutoFlush = true };

        try
        {
            string? line;
            while (!cts.IsCancellationRequested && (line = await reader.ReadLineAsync()) != null)
            {
                if (string.IsNullOrWhiteSpace(line)) continue;

                JsonElement request;
                try { request = JsonSerializer.Deserialize<JsonElement>(line); }
                catch { continue; }

                string method = request.TryGetProperty("method", out var m) ? m.GetString() ?? "" : "";

                if (method == "initialize")
                {
                    string protocolVersion =
                        request.TryGetProperty("params", out var initParams)
                        && initParams.TryGetProperty("protocolVersion", out var pv)
                        ? pv.GetString() ?? "2024-11-05" : "2024-11-05";
                    var response = new
                    {
                        jsonrpc = "2.0",
                        id = request.GetProperty("id").GetInt32(),
                        result = new
                        {
                            protocolVersion,
                            capabilities = new { tools = new { } },
                            serverInfo = new { name = "cnet-mcp", version = "0.2.0" }
                        }
                    };
                    await writer.WriteLineAsync(JsonSerializer.Serialize(response));
                }
                else if (method == "tools/list")
                {
                    var response = new
                    {
                        jsonrpc = "2.0",
                        id = request.GetProperty("id").GetInt32(),
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
                                }
                            }
                        }
                    };
                    await writer.WriteLineAsync(JsonSerializer.Serialize(response));
                }
                else if (method == "tools/call")
                {
                    var toolName = request.GetProperty("params").GetProperty("name").GetString();
                    var toolArgs = request.GetProperty("params").GetProperty("arguments");

                    // A tool failure must never kill the stdio server: report
                    // it as the tool result instead of unwinding the read loop.
                    string resultText;
                    try
                    {
                        resultText = toolName switch
                    {
                        "cnet_verify_claim" => tools.VerifyClaim(
                            toolArgs.GetProperty("claim").GetString() ?? "",
                            toolArgs.TryGetProperty("unitTag", out var ut) ? ut.GetString() ?? "" : "",
                            ReadStringList(toolArgs, "codebaseNodes"),
                            ReadStringList(toolArgs, "counterfactualRoutes"),
                            ReadOptionalDouble(toolArgs, "counterfactualConsistency")),
                        "cnet_generate_testimony" => tools.GenerateTestimony(
                            toolArgs.GetProperty("place").GetString() ?? "",
                            toolArgs.GetProperty("dilemma").GetString() ?? "",
                            toolArgs.GetProperty("consequence").GetString() ?? "",
                            ReadStringList(toolArgs, "codebaseNodes")),
                        "cnet_compress_model" => tools.CompressModel(
                            toolArgs.GetProperty("model_path").GetString() ?? "",
                            toolArgs.TryGetProperty("target_size", out var ts) ? ts.GetString() ?? "1.6bit" : "1.6bit",
                            toolArgs.TryGetProperty("options", out var co) ? co.GetString() ?? "" : ""),
                        "cnet_expand_context" => tools.ExpandContext(
                            toolArgs.GetProperty("input").GetString() ?? "",
                            toolArgs.TryGetProperty("baseDim", out var bd) ? bd.GetInt32() : 8192),
                        "cnet_route_on_role" => tools.RouteOnRole(
                            toolArgs.GetProperty("input").GetString() ?? "",
                            toolArgs.TryGetProperty("role", out var r) ? r.GetString() ?? "memory-witness" : "memory-witness"),
                        "cnet_list_units" => tools.ListUnits(),
                        _ => "Unknown tool: " + toolName
                    };
                    }
                    catch (Exception toolExc)
                    {
                        Console.Error.WriteLine($"[CNET MCP] tool '{toolName}' failed: {toolExc}");
                        resultText = $"[CNET] Tool '{toolName}' failed: {toolExc.Message}";
                    }

                    var response = new
                    {
                        jsonrpc = "2.0",
                        id = request.GetProperty("id").GetInt32(),
                        result = new { content = new[] { new { type = "text", text = resultText } } }
                    };
                    await writer.WriteLineAsync(JsonSerializer.Serialize(response));
                }
            }
        }
        catch (OperationCanceledException)
        {
            Console.Error.WriteLine("[CNET MCP] Graceful shutdown complete.");
        }
    }

    private static List<string> ReadStringList(JsonElement args, string propertyName)
    {
        var values = new List<string>();
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

    private static double? ReadOptionalDouble(JsonElement args, string propertyName)
    {
        if (!args.TryGetProperty(propertyName, out var element) ||
            element.ValueKind != JsonValueKind.Number)
        {
            return null;
        }
        return element.GetDouble();
    }
}