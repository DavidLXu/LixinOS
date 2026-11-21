"""Apply the small LixinOS adaptations to the pinned upstream source copy."""
from pathlib import Path
import sys
src = Path(sys.argv[1])


def replace(name, before, after):
    p = src / name
    text = p.read_text()
    if text.count(before) != 1:
        raise SystemExit(f"unexpected upstream {name}: {before[:55]}")
    p.write_text(text.replace(before, after))


p = src / "Platform_Posix.c"
text = p.read_text()
start = text.index("cc_result res;", text.index("int main(int argc, char** argv)"))
end = text.index("\n}", start)
p.write_text(text[:start]+'''extern int lixin_game_open(int, char**);
    if (!lixin_game_open(argc, argv)) return 1;
    if (mkdir("ClassiCube", 0777) < 0 && errno != EEXIST) { perror("ClassiCube"); return 1; }
    if (chdir("ClassiCube") < 0) { perror("ClassiCube"); return 1; }
    SetupProgram(argc, argv);
    Options_Get(LOPT_USERNAME, &Game_Username, DEFAULT_USERNAME);
    RunGame();
    Window_Free();
    return 0;'''+text[end:])
replace("main_impl.h", "res = Platform_SetDefaultCurrentDirectory();", "res = 0;")
replace("TexturePack.c", 'defaults[2] = "texpacks/classicube.zip";',
        'defaults[2] = "/lib/classicube/textures.zip";')

replace("Platform_Posix.c", "union SocketAddress {", "#ifdef CC_BUILD_NETWORKING\nunion SocketAddress {")
replace("Platform_Posix.c", "void Socket_Close(cc_socket s) {", "#endif /* CC_BUILD_NETWORKING */\nvoid Socket_Close(cc_socket s) {")
replace("Game.c", "Options_GetBool(OPT_CLASSIC_MODE,  false)", "Options_GetBool(OPT_CLASSIC_MODE, true)")
replace("Game.h", "#define DEFAULT_VIEWDIST 512", "#define DEFAULT_VIEWDIST 64")
replace("Server.c", "gen = &NotchyGen;", "extern int lixin_flat_world;\n\tgen = lixin_flat_world ? &FlatgrassGen : &NotchyGen;")
replace("TexturePack.c", "path = String_FromReadonly(defaults[i]);",
        "path = String_FromReadonly(defaults[i]);\n\t\tcc_filepath candidate;\n"
        "\t\tPlatform_EncodePath(&candidate, &path);\n\t\tif (!File_Exists(&candidate)) continue;")
replace("Game.c", "Options_GetEnum(OPT_FPS_LIMIT, 0,", "Options_GetEnum(OPT_FPS_LIMIT, FPS_LIMIT_60,")
