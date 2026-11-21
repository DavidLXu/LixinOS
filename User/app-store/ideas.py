"""Read-only idea generation through the desktop worker model bridge."""
def store_ideas(seed, request):
    import re
    if not re.fullmatch(r"[0-9]{1,10}", seed):
        raise ValueError("invalid idea seed")
    instruction = ('Suggest three distinct small native desktop applications for LixinOS. '
                   'Each suggestion is one actionable Chinese sentence, at most 32 characters, '
                   'with a specific interaction. Return JSON with tool "finish" and args '
                   'containing the three suggestions. No tools or file access.')
    result = request("chat-session", instruction, "Generate fresh ideas. Variation " + seed,
                     "/", "0", "", "", "", "", retry=False)
    if (len(result) != 4 or result[0] != "finish" or len(set(result[1:])) != 3 or
            any(not text.strip() or len(text) > 64 or not text.isprintable() for text in result[1:])):
        raise ValueError("invalid idea response")
    for text in result[1:]:
        print("IDEA\t" + text)
