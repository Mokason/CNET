using System;
using System.Text;
using System.Threading.Tasks;
using CNET.Cce.CnetHarness;

namespace CNET.CceHost;

/// <summary>
/// Chat client backed by the CNET .NET inference harness. Assembles the
/// role/content transcript into a single system+user pair (the first system
/// message becomes System; the concatenated user/assistant turns become
/// User), because the current harness ABI is synchronous non-streaming and
/// takes exactly one system + user prompt per generate call.
/// </summary>
public sealed class CnetHarnessChatClient : IChatClient, IDisposable
{
    private readonly CnetHarnessSession _session;
    private readonly string _role;
    private readonly uint _maxTokens;
    private readonly bool _ownsSession;

    public CnetHarnessChatClient(CnetHarnessSession session, string role,
                                  uint maxTokens = 512, bool ownsSession = true)
    {
        ArgumentNullException.ThrowIfNull(session);
        ArgumentException.ThrowIfNullOrEmpty(role);
        _session = session;
        _role = role;
        _maxTokens = maxTokens;
        _ownsSession = ownsSession;
    }

    public Task<string> ChatAsync((string Role, string Content)[] messages)
    {
        StringBuilder systemBuf = new();
        StringBuilder userBuf = new();
        foreach (var m in messages)
        {
            if (string.Equals(m.Role, "system", StringComparison.OrdinalIgnoreCase))
            {
                if (systemBuf.Length > 0) systemBuf.Append('\n');
                systemBuf.Append(m.Content);
            }
            else
            {
                if (userBuf.Length > 0) userBuf.Append('\n');
                userBuf.Append(m.Role).Append(": ").Append(m.Content);
            }
        }
        var options = new CnetHarnessGenerateOptions
        {
            System = systemBuf.Length > 0 ? systemBuf.ToString() : null,
            User = userBuf.ToString(),
            Role = _role,
            MaxTokens = _maxTokens,
            Sampling = CnetHarnessSamplingMode.Auto,
        };
        var result = _session.Generate(options);
        return Task.FromResult(result.Text);
    }

    public void Dispose()
    {
        if (_ownsSession) _session.Dispose();
    }
}
