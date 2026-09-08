using System;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using Xunit;

namespace CnetMcpServer.Tests;

public sealed class SafeWebPolicyTests
{
    [Theory]
    [InlineData("0.1.2.3")]
    [InlineData("10.0.0.1")]
    [InlineData("100.64.0.1")]
    [InlineData("100.127.255.254")]
    [InlineData("127.0.0.1")]
    [InlineData("169.254.169.254")]
    [InlineData("172.16.0.1")]
    [InlineData("172.31.255.255")]
    [InlineData("192.0.0.9")]
    [InlineData("192.0.2.1")]
    [InlineData("192.88.99.1")]
    [InlineData("192.168.0.1")]
    [InlineData("198.18.0.1")]
    [InlineData("198.19.255.255")]
    [InlineData("198.51.100.1")]
    [InlineData("203.0.113.1")]
    [InlineData("224.0.0.1")]
    [InlineData("240.0.0.1")]
    [InlineData("255.255.255.255")]
    [InlineData("::")]
    [InlineData("::1")]
    [InlineData("::ffff:8.8.8.8")]
    [InlineData("::ffff:127.0.0.1")]
    [InlineData("64:ff9b::a00:1")]
    [InlineData("fc00::1")]
    [InlineData("fd00::1")]
    [InlineData("fe80::1")]
    [InlineData("ff02::1")]
    [InlineData("2001::1")]
    [InlineData("2001:2::1")]
    [InlineData("2001:db8::1")]
    [InlineData("2002:7f00:1::1")]
    [InlineData("3fff::1")]
    [InlineData("4000::1")]
    public void RefusesNonPublicAddresses(string value) => Assert.False(SafeWebPolicy.IsPublicAddress(IPAddress.Parse(value)));

    [Theory]
    [InlineData("8.8.8.8")]
    [InlineData("1.1.1.1")]
    [InlineData("208.80.154.224")]
    [InlineData("2606:4700:4700::1111")]
    [InlineData("2001:4860:4860::8888")]
    public void AcceptsPublicAddresses(string value) => Assert.True(SafeWebPolicy.IsPublicAddress(IPAddress.Parse(value)));

    [Theory]
    [InlineData("http://example.org/")]
    [InlineData("https://example.org:444/")]
    [InlineData("https://user:pass@example.org/")]
    [InlineData("https://example.org/#fragment")]
    [InlineData("https://example.org/#")]
    [InlineData("https://127.0.0.1/")]
    [InlineData("https://2130706433/")]
    [InlineData("https://[::1]/")]
    [InlineData("https://localhost/")]
    [InlineData("https://sub.example.org/")]
    [InlineData("https://example.org.evil.test/")]
    [InlineData("https://example.org./")]
    [InlineData("https://example.org%2e/")]
    [InlineData("https://example.org\\@evil.test/")]
    [InlineData("https://example.org/\nsecret")]
    [InlineData("file:///etc/passwd")]
    public void RefusesUnsafeUrls(string url)
        => Assert.Throws<SafeWebRefusal>(() => new SafeWebPolicy(["example.org"]).ValidateUrl(url));

    [Theory]
    [InlineData("")]
    [InlineData("*.example.org")]
    [InlineData("https://example.org")]
    [InlineData("example.org.")]
    [InlineData("127.0.0.1")]
    [InlineData("service.local")]
    [InlineData("service.internal")]
    [InlineData("éxample.org")]
    public void InvalidAllowlistFailsClosed(string host) => Assert.False(new SafeWebPolicy([host]).IsValid);

    [Fact]
    public async Task MixedDnsResponseNeverConnects()
    {
        bool connected = false;
        var policy = new SafeWebPolicy(["example.org"]);
        var error = await Assert.ThrowsAsync<SafeWebRefusal>(async () => await policy.ConnectAsync("example.org", 443,
            CancellationToken.None, (_, _) => Task.FromResult(new[] { IPAddress.Parse("8.8.8.8"), IPAddress.Loopback }),
            (_, _) => { connected = true; return ValueTask.FromResult<Stream>(new MemoryStream()); }));
        Assert.Equal("dns_refused", error.Code);
        Assert.False(connected);
    }

    [Fact]
    public async Task VettedAddressIsPinnedWithoutSecondResolution()
    {
        int resolutions = 0;
        IPEndPoint? endpoint = null;
        var policy = new SafeWebPolicy(["example.org"]);
        using var stream = await policy.ConnectAsync("example.org", 443, CancellationToken.None,
            (_, _) => Task.FromResult(new[] { ++resolutions == 1 ? IPAddress.Parse("8.8.8.8") : IPAddress.Loopback }),
            (value, _) => { endpoint = value; return ValueTask.FromResult<Stream>(new MemoryStream()); });
        Assert.Equal(1, resolutions);
        Assert.Equal("8.8.8.8", endpoint!.Address.ToString());
        Assert.Equal(443, endpoint.Port);
    }

    [Fact]
    public void HandlerCannotProxyRedirectAuthenticateOrSkipTlsValidation()
    {
        using var handler = new SafeWebPolicy(["example.org"]).CreateHandler();
        Assert.False(handler.AllowAutoRedirect);
        Assert.False(handler.UseProxy);
        Assert.False(handler.UseCookies);
        Assert.False(handler.PreAuthenticate);
        Assert.Null(handler.Credentials);
        Assert.Equal(DecompressionMethods.None, handler.AutomaticDecompression);
        Assert.Null(handler.SslOptions.RemoteCertificateValidationCallback);
        Assert.Null(handler.SslOptions.ClientCertificates);
        Assert.NotNull(handler.ConnectCallback);
    }
}
