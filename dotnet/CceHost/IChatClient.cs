using System.Threading.Tasks;

namespace CNET.CceHost;

/// <summary>Small chat-client abstraction so agents can pick a backend.</summary>
public interface IChatClient
{
    /// <summary>Send a role/content transcript, return the assistant's text.</summary>
    Task<string> ChatAsync((string Role, string Content)[] messages);
}
