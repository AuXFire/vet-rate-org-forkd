"""The Unreal source checker must pass on the project and must actually catch the breakage it claims to."""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "Tools"))
import check_unreal  # noqa: E402


def test_project_sources_pass():
    assert check_unreal.main() == 0


def test_common_breakage_is_caught(tmp_path, monkeypatch):
    module = tmp_path / "Source" / "Undertow"
    shutil.copytree(os.path.join(ROOT, "Source", "Undertow"), module)
    monkeypatch.setattr(check_unreal, "ROOT", str(tmp_path))
    monkeypatch.setattr(check_unreal, "MODULE", str(module))

    def edit(name, old, new):
        path = module / "Game" / name
        text = path.read_text()
        assert old in text, "%s no longer contains %r" % (name, old)
        path.write_text(text.replace(old, new, 1))

    edit("UndertowVehicle.cpp", "void AUndertowVehicle::ExitDriver()", "void AUndertowVehicle::ExitDriverRenamed()")
    edit("UndertowCharacter.h", '#include "UndertowCharacter.generated.h"\n', "")
    edit("UndertowGameMode.cpp", '#include "Game/UndertowCharacter.h"', '#include "Game/NoSuchFile.h"')
    edit("UndertowSaveGame.h", "class UNDERTOW_API UUndertowSaveGame", "class UUndertowSaveGame")
    edit("UndertowHUDWidget.h", '#include "UndertowHUDWidget.generated.h"\n\nclass UUndertowSimSubsystem;',
         '#include "UndertowHUDWidget.generated.h"\n#include "Game/UndertowTypes.h"\n\nclass UUndertowSimSubsystem;')
    edit("UndertowWeaponComponent.cpp", "void UUndertowWeaponComponent::Reload()\n{", "void UUndertowWeaponComponent::Reload()\n{{")

    problems = []
    for folder, _, files in os.walk(module):
        for f in sorted(files):
            if f.endswith((".h", ".cpp")) and os.sep + "Sim" + os.sep not in os.path.join(folder, f):
                check_unreal.check_file(os.path.join(folder, f), problems)
    text = "\n".join(problems)
    assert "AUndertowVehicle::ExitDriver is declared but not defined" in text
    assert "UndertowCharacter.h:1: error: reflected type but no UndertowCharacter.generated.h include" in text
    assert "includes missing file Game/NoSuchFile.h" in text
    assert "class UUndertowSaveGame has no UNDERTOW_API export macro" in text
    assert "UndertowHUDWidget.generated.h must be the last #include" in text
    assert "UndertowWeaponComponent.cpp:1: error: unbalanced {}" in text
    assert len(problems) == 6, text
