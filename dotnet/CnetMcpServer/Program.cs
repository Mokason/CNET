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
                                new { name = "cnet_verify_claim", description = "Verify a claim against CNET units with optional counterfactual route evidence" },
                                new { name = "cnet_generate_testimony", description = "Generate Memory-Witness testimony with narrative coherence scoring (place + dilemma + consequence)" },
                                new { name = "cnet_compress_model", description = "Prepare a CNET-compressed model wrapper for Hermes hosting" },
                                new { name = "cnet_expand_context", description = "Expand context with AICIMO routing + uncertainty" },
                                new { name = "cnet_route_on_role", description = "Route using AICIMO role-slice (Drole)" },
                                new { name = "cnet_list_units", description = "List available CNET units" }
                            }
                        }
                    };
                    await writer.WriteLineAsync(JsonSerializer.Serialize(response));
                }
                else if (method == "tools/call")
                {
                    var toolName = request.GetProperty("params").GetProperty("name").GetString();
                    var toolArgs = request.GetProperty("params").GetProperty("arguments");

                    string resultText = toolName switch
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