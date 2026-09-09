using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;

// Development probe only. Invoke the actual hash-pinned managed parser without
// changing its visibility, starting the control program, or creating a ledger.
// Corpus strings are data passed to Propose, never commands or reflection names.
try
{
    if (args.Length != 2 || !Path.IsPathFullyQualified(args[0]) ||
        args[1].Length != 64 || args[1].Any(c => !char.IsAsciiHexDigitLower(c)))
        throw new InvalidDataException();
    using var assemblyFile = File.OpenRead(args[0]);
    var assemblyBytes = ReadBounded(assemblyFile, 32 * 1024 * 1024);
    var identity = Convert.ToHexString(SHA256.HashData(assemblyBytes)).ToLowerInvariant();
    if (identity != args[1]) throw new InvalidDataException();
    using var input = Console.OpenStandardInput();
    // At most 128 requests x 4096 UTF-16 units; numeric JSON is bounded to 4 MiB.
    // Units preserve malformed surrogate input for the real parser to refuse.
    using var document = JsonDocument.Parse(ReadBounded(input, 4 * 1024 * 1024), new JsonDocumentOptions { MaxDepth = 4 });
    if (document.RootElement.ValueKind != JsonValueKind.Array || document.RootElement.GetArrayLength() is < 1 or > 128)
        throw new InvalidDataException();
    var requests = document.RootElement.EnumerateArray().Select(item =>
    {
        if (item.ValueKind != JsonValueKind.Array || item.GetArrayLength() > 4096) throw new InvalidDataException();
        return new string(item.EnumerateArray().Select(unit =>
            unit.ValueKind == JsonValueKind.Number && unit.TryGetUInt16(out var value)
                ? (char)value : throw new InvalidDataException()).ToArray());
    }).ToArray();
    // Load exactly the bytes hashed above. No path reopen or entry-point call.
    var assembly = Assembly.Load(assemblyBytes);
    var type = assembly.GetType("CnetControlPlane.Learning.LearningTaskParser", throwOnError: true)!;
    var propose = type.GetMethod("Propose", BindingFlags.NonPublic | BindingFlags.Static,
        binder: null, types: [typeof(string)], modifiers: null) ?? throw new InvalidDataException();
    var proposals = requests.Select(text =>
    {
        var result = propose.Invoke(null, [text]) ?? throw new InvalidDataException();
        return JsonSerializer.SerializeToElement(result, result.GetType());
    }).ToArray();
    Console.WriteLine(JsonSerializer.Serialize(new { schema = 1, assembly_sha256 = identity, proposals }));
    return 0;
}
catch
{
    Console.Error.WriteLine("PARAPHRASE_PROBE_REFUSED");
    return 2;
}

static byte[] ReadBounded(Stream stream, int limit)
{
    using var output = new MemoryStream();
    var buffer = new byte[8192];
    int read;
    while ((read = stream.Read(buffer, 0, Math.Min(buffer.Length, limit + 1 - (int)output.Length))) > 0)
    {
        output.Write(buffer, 0, read);
        if (output.Length > limit) throw new InvalidDataException();
    }
    return output.ToArray();
}
