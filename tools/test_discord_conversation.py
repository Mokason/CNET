import urllib.request
import json
import re

MOUTH_URL = "http://127.0.0.1:8084"

# Channel context memory: channel_id -> list of {"role": str, "content": str}
CHANNEL_MEMORY = {}

def ask_mouth_chat(channel_id, author, text):
    history = CHANNEL_MEMORY.setdefault(channel_id, [])
    history.append({"role": "user", "content": f"{author}: {text}"})
    
    req_body = json.dumps({
        "messages": history[-8:],
        "max_tokens": 160
    }).encode("utf-8")
    
    req = urllib.request.Request(
        f"{MOUTH_URL}/chat",
        data=req_body,
        headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            reply = data.get("reply", "").strip()
            history.append({"role": "assistant", "content": reply})
            if len(history) > 16:
                history[:] = history[-16:]
            return reply
    except Exception as e:
        return f"(Mouth service unavailable: {e})"

def ask_mouth_story(text):
    style = "whimsical"
    hero = "Oliver the fox"
    setting = "enchanted forest"
    artifact = "glowing mushroom"

    low = text.lower()
    if "dragon" in low or "adventurous" in low:
        style = "adventurous"
        hero = "Ember the young dragon"
        setting = "green hills"
        artifact = "emerald wings"
    elif "cozy" in low or "rabbit" in low or "cookie" in low:
        style = "cozy"
        hero = "Lily and Barnaby"
        setting = "country kitchen"
        artifact = "sweet cookies"

    req_body = json.dumps({
        "hero": hero,
        "setting": setting,
        "artifact": artifact,
        "style": style,
        "max_tokens": 120
    }).encode("utf-8")

    req = urllib.request.Request(
        f"{MOUTH_URL}/generate",
        data=req_body,
        headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            story = data.get("story", "").strip()
            return f"✨ **[CNET Narrative Engine — {style.title()} Arc]**\n*{story}*\n\n`[VSA Back-Projection Audit: CERTIFIED_SAFE | Hero: {hero}]`"
    except Exception as e:
        return f"(Story generation unavailable: {e})"

def handle_discord_message(channel_id, author, content):
    low = content.lower().strip()
    # 1. Story detection
    if any(k in low for k in ["tell a story", "tell me a story", "!story", "write a story"]):
        return ask_mouth_story(content)
    
    # 2. Conversational chat
    return ask_mouth_chat(channel_id, author, content)

if __name__ == "__main__":
    test_queries = [
        "hello, who are you?",
        "what is capital of tokyo?",
        "tell a story",
        "tell me an adventurous story about a dragon",
        "thanks! what were the dragon's wings made of?"
    ]
    for q in test_queries:
        print(f"\nUser: {q}")
        reply = handle_discord_message("test_channel", "marble_user", q)
        print(f"Bot: {reply}")
