using System.Net;
using System.Net.Sockets;

namespace CnetMcpServer;

internal sealed class SafeWebRefusal(string code) : Exception(code)
{
    internal string Code { get; } = code;
}

/// <summary>Policy is rechecked at the socket boundary, not only before DNS.</summary>
internal sealed class SafeWebPolicy
{
    private readonly HashSet<string> _hosts = new(StringComparer.OrdinalIgnoreCase);
    internal bool IsValid { get; }

    internal SafeWebPolicy(IEnumerable<string> hosts)
    {
        foreach (var host in hosts)
        {
            if (_hosts.Count >= 32 || !IsDnsHost(host)) return;
            _hosts.Add(host);
        }
        IsValid = _hosts.Count > 0;
    }

    internal bool AllowsHost(string host) => IsValid && IsDnsHost(host) && _hosts.Contains(host);

    private static bool IsDnsHost(string host)
    {
        if (host.Length is < 3 or > 253 || !host.Contains('.') || IPAddress.TryParse(host, out _)) return false;
        foreach (var label in host.Split('.'))
        {
            if (label.Length is < 1 or > 63 || label[0] == '-' || label[^1] == '-') return false;
            if (label.Any(c => !char.IsAsciiLetterOrDigit(c) && c != '-')) return false;
        }
        return !host.EndsWith(".localhost", StringComparison.OrdinalIgnoreCase)
            && !host.EndsWith(".local", StringComparison.OrdinalIgnoreCase)
            && !host.EndsWith(".internal", StringComparison.OrdinalIgnoreCase);
    }

    internal Uri ValidateUrl(string raw)
    {
        if (raw.Length is < 1 or > 2048 || raw.Any(char.IsControl) || raw.Contains('\\') || raw.Contains('#')
            || !Uri.TryCreate(raw, UriKind.Absolute, out var uri) || uri.Scheme != "https"
            || uri.Port != 443 || uri.UserInfo.Length != 0 || uri.HostNameType != UriHostNameType.Dns
            || !AllowsHost(uri.Host) || uri.AbsoluteUri.Length > 2048)
            throw new SafeWebRefusal("url_refused");
        // Reject URI parser normalization of escaped or whitespace-bearing authorities.
        var authority = raw.AsSpan(raw.IndexOf("://", StringComparison.Ordinal) + 3);
        var end = authority.IndexOfAny('/', '?');
        if (end >= 0) authority = authority[..end];
        foreach (char c in authority)
            if (c > 127 || char.IsWhiteSpace(c) || c is '%' or '@') throw new SafeWebRefusal("url_refused");
        return uri;
    }

    // Conservative public-unicast policy: also refuses special-purpose ranges that
    // have narrow public exceptions. Do not expand these ranges to make a fetch pass.
    // Reviewed 2026-09-08 against IANA's iana-ipv4-special-registry and
    // iana-ipv6-special-registry (both last updated 2025-10-09).
    internal static bool IsPublicAddress(IPAddress address)
    {
        var b = address.GetAddressBytes();
        if (address.AddressFamily == AddressFamily.InterNetwork)
            return !(b[0] is 0 or 10 or 127 || b[0] >= 224
                || (b[0] == 100 && b[1] is >= 64 and <= 127)
                || (b[0] == 169 && b[1] == 254)
                || (b[0] == 172 && b[1] is >= 16 and <= 31)
                || (b[0] == 192 && (b[1] == 168 || b[1] == 0
                    || (b[1] == 88 && b[2] == 99)))
                || (b[0] == 198 && (b[1] is 18 or 19 || (b[1] == 51 && b[2] == 100)))
                || (b[0] == 203 && b[1] == 0 && b[2] == 113));
        if (address.AddressFamily != AddressFamily.InterNetworkV6 || address.IsIPv4MappedToIPv6
            || address.ScopeId != 0 || (b[0] & 0xe0) != 0x20) return false;
        return !(b[0] == 0x20 && b[1] == 0x01 && b[2] < 2) // IETF special-purpose /23
            && !(b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8)
            && !(b[0] == 0x20 && b[1] == 0x02) // 6to4 may embed an unsafe IPv4 destination
            && !(b[0] == 0x3f && b[1] == 0xff && (b[2] & 0xf0) == 0); // documentation /20
    }

    internal async ValueTask<Stream> ConnectAsync(string host, int port, CancellationToken cancellationToken,
        Func<string, CancellationToken, Task<IPAddress[]>> resolver,
        Func<IPEndPoint, CancellationToken, ValueTask<Stream>> connector)
    {
        if (port != 443 || !AllowsHost(host)) throw new SafeWebRefusal("url_refused");
        var addresses = await resolver(host, cancellationToken).ConfigureAwait(false);
        if (addresses.Length is < 1 or > 32 || addresses.Any(a => !IsPublicAddress(a)))
            throw new SafeWebRefusal("dns_refused");
        // One resolution, then an IP endpoint: the socket never resolves the name again.
        var selected = addresses.FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork) ?? addresses[0];
        return await connector(new IPEndPoint(selected, port), cancellationToken).ConfigureAwait(false);
    }

    internal SocketsHttpHandler CreateHandler() => new()
    {
        AllowAutoRedirect = false,
        UseProxy = false,
        UseCookies = false,
        Credentials = null,
        PreAuthenticate = false,
        AutomaticDecompression = DecompressionMethods.None,
        MaxConnectionsPerServer = 2,
        MaxResponseHeadersLength = 16,
        PooledConnectionLifetime = TimeSpan.Zero,
        ConnectTimeout = TimeSpan.FromSeconds(10),
        ConnectCallback = (context, token) => ConnectAsync(context.DnsEndPoint.Host, context.DnsEndPoint.Port,
            token, (host, ct) => Dns.GetHostAddressesAsync(host, ct), ConnectSocketAsync)
        // Default TLS certificate and hostname validation intentionally remain intact.
    };

    private static async ValueTask<Stream> ConnectSocketAsync(IPEndPoint endpoint, CancellationToken token)
    {
        var socket = new Socket(endpoint.AddressFamily, SocketType.Stream, ProtocolType.Tcp) { NoDelay = true };
        try
        {
            await socket.ConnectAsync(endpoint, token).ConfigureAwait(false);
            return new NetworkStream(socket, ownsSocket: true);
        }
        catch { socket.Dispose(); throw; }
    }
}
