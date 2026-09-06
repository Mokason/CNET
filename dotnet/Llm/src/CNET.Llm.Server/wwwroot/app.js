'use strict';
(() => {
  const byId = id => document.getElementById(id);
  const input = byId('inference-key');
  const prompt = byId('message');
  const messages = byId('messages');
  const status = byId('status');
  let key = '';
  let accessSet = false;
  let accessGeneration = 0;
  let controller = null;
  let history = [];

  function controls() {
    byId('send').disabled = !accessSet || controller !== null;
    byId('stop').disabled = controller === null;
    byId('new-chat').disabled = controller !== null;
    prompt.disabled = controller !== null;
    messages.setAttribute('aria-busy', String(controller !== null));
  }
  function clearChat() {
    history = [];
    messages.replaceChildren();
    prompt.value = '';
    byId('empty-state').hidden = false;
  }
  function setAccess(value) {
    accessGeneration++;
    controller?.abort();
    key = value;
    accessSet = true;
    input.value = '';
    clearChat();
    status.textContent = value ? 'Inference key is held in memory. Ready to send.' : 'Local development selected. The server must explicitly allow it.';
    controls();
    prompt.focus();
  }
  byId('access-form').addEventListener('submit', event => {
    event.preventDefault();
    if (!/^[\x21-\x7e]{32,256}$/.test(input.value)) {
      status.textContent = 'Enter a valid 32–256 character inference key.';
      input.focus();
      return;
    }
    setAccess(input.value);
  });
  byId('development').addEventListener('click', () => setAccess(''));
  byId('forget-key').addEventListener('click', () => {
    accessGeneration++;
    controller?.abort();
    key = '';
    accessSet = false;
    input.value = '';
    clearChat();
    controls();
    status.textContent = 'Key and conversation forgotten.';
    input.focus();
  });
  byId('new-chat').addEventListener('click', () => { clearChat(); status.textContent = 'New conversation. Ready to send.'; prompt.focus(); });
  byId('stop').addEventListener('click', () => controller?.abort());

  function addMessage(role, text) {
    const article = document.createElement('article');
    article.className = `message ${role}`;
    const label = document.createElement('p');
    label.className = 'message-role';
    label.textContent = role === 'user' ? 'You' : 'Model';
    const body = document.createElement('p');
    body.className = 'message-text';
    body.textContent = text;
    article.append(label, body);
    messages.append(article);
    while (messages.children.length > 24) messages.firstElementChild.remove();
    byId('empty-state').hidden = true;
    return body;
  }
  function failure(code) {
    return ({ 400: 'Request refused. Shorten the conversation or start a new chat.',
      401: 'Access denied. Set a valid inference key.', 403: 'Access or origin denied. Ask the server owner.',
      413: 'Request too large. Shorten the message or start a new chat.',
      429: 'Server busy or rate limit reached. Wait before trying again.',
      503: 'No model is ready. Ask the server owner.', 504: 'Request deadline reached. The reply is incomplete.' })[code]
      || 'Request failed. The reply is incomplete.';
  }
  byId('chat-form').addEventListener('submit', async event => {
    event.preventDefault();
    const text = prompt.value.trim();
    if (!accessSet || controller || !text || text.length > 8192) return;
    const turns = [...history, { role: 'user', content: text }];
    const payload = JSON.stringify({ messages: turns, stream: true });
    if (new TextEncoder().encode(payload).length > 60000) { status.textContent = 'Conversation too large. Start a new chat.'; return; }
    const active = new AbortController();
    const generation = accessGeneration;
    controller = active;
    controls();
    addMessage('user', text);
    const reply = addMessage('assistant', '');
    status.textContent = 'Generating…';
    let output = '';
    let done = false;
    try {
      const headers = { 'Content-Type': 'application/json' };
      if (key) headers.Authorization = `Bearer ${key}`;
      const response = await fetch('/v1/chat/completions', {
        method: 'POST', headers, body: payload, signal: active.signal,
        credentials: 'omit', cache: 'no-store', redirect: 'error',
      });
      if (!response.ok) { status.textContent = failure(response.status); return; }
      if (!response.headers.get('content-type')?.startsWith('text/event-stream') || !response.body) throw new Error('Invalid stream');
      const reader = response.body.getReader();
      const decoder = new TextDecoder();
      let pending = '';
      try {
        while (!done) {
          const chunk = await reader.read();
          if (chunk.done) break;
          pending += decoder.decode(chunk.value, { stream: true });
          if (pending.length > 65536) throw new Error('Stream budget exceeded');
          let boundary;
          while ((boundary = pending.indexOf('\n\n')) !== -1) {
            const event = pending.slice(0, boundary);
            pending = pending.slice(boundary + 2);
            for (const line of event.split('\n')) {
              if (!line.startsWith('data: ')) continue;
              const data = line.slice(6);
              if (data === '[DONE]') { done = true; break; }
              const delta = JSON.parse(data)?.choices?.[0]?.delta?.content;
              if (delta !== undefined && typeof delta !== 'string') throw new Error('Invalid content');
              output += delta || '';
              if (output.length > 65536) throw new Error('Reply budget exceeded');
              reply.textContent = output;
            }
          }
        }
      } finally { await reader.cancel(); reader.releaseLock(); }
      if (generation !== accessGeneration) return;
      if (!done) throw new Error('Incomplete stream');
      history = [...turns, { role: 'assistant', content: output }].slice(-12);
      while (history.reduce((size, turn) => size + turn.content.length, 0) > 24000) history.shift();
      prompt.value = '';
      status.textContent = 'Reply complete.';
    } catch {
      if (generation === accessGeneration)
        status.textContent = active.signal.aborted ? 'Stopped. The reply is incomplete.' : 'Connection or stream failed. The reply is incomplete.';
    } finally {
      active.abort();
      controller = null;
      controls();
      if (accessSet) prompt.focus();
    }
  });
  controls();
})();
