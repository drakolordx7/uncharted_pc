// SPDX-License-Identifier: GPL-2.0-or-later
// U1 (English, French, Spanish, Portuguese): tutorial hints written for a controller, reworded in memory, at run time, so that the text
// and the key icon say the same thing on a keyboard and a mouse. The game builds each icon from a token of
// the text (|LA|, |RA|, |R3|...), so a hint can get the icon that fits it:
//   "Click left |A| to switch shoulders"  drew the stick (WASD) for the click of the stick (L3, a key of its own)
//   "Use left |A| to move Drake"           "left" is meaningless beside the WASD
//   "Press |D-PAD-LR| to change weapons"   the icon is the mouse wheel here (DrawWheelFrame)
// A reworded text is padded with spaces to the length of the original: the text bank is a run of
// NUL-terminated strings, and nothing after the one that changed may move.
#include "bbport_keyicons.h"

#ifdef _WIN32
#include <windows.h> // NOMINMAX and WIN32_LEAN_AND_MEAN come from the build
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace BbKeyIcons {

#ifdef _WIN32

namespace {

struct Rewording {
    const char* original;
    const char* replacement; // not longer than the original
};

// "_" in the game's texts stands for a space that does not break the line: kept as is.
// The jetski: on it the keys of forward and back (W and S by default; src/runtime_pad.c, u1_on_jetski) are the throttle
// and the brake, and the atlas paints the cross and square icons with them while Elena rides (SetJetskiMode), so each
// hint keeps only that one icon (|X|: forward, |S|: back) and loses the controller's second button.
constexpr Rewording Rewordings[] = {
    {"Hold |X| or |R2| to accelerate", "Hold |X| to accelerate"},
    {"Hold |C| or |S| to brake and reverse", "Hold |S| to brake and reverse"},
    {"Maintenir |X| ou |R2| enfoncée pour accélérer", "Maintenir |X| enfoncée pour accélérer"},
    {"Maintenir |C| ou |S| enfoncée pour freiner et faire marche arrière",
     "Maintenir |S| enfoncée pour freiner et faire marche arrière"},

    {"Click left |A| to switch shoulders", "Press |L3| to switch shoulders"},
    {"Click right |A| to point camera where Drake is_looking", "Press |R3| to point camera where Drake is_looking"},
    {"Use left |A| to move Drake", "Use |LA| to move Drake"},
    {"Use right |A| to move the camera", "Use |RA| to move the camera"},
    {"Press |D-PAD-LR| to change weapons", "Use |D-PAD-LR| to change weapons"},
    // French
    {"Appuyer sur |A| gauche pour changer d'épaule", "Appuyer sur |L3| pour changer d'épaule"},
    {"Appuyer sur |A| droit pour orienter la caméra là où regarde_Drake",
     "Appuyer sur |R3| pour orienter la caméra là où regarde_Drake"},
    {"Utiliser |A| gauche pour déplacer Drake", "Utiliser |LA| pour déplacer Drake"},
    {"Utiliser |A| droit pour déplacer la caméra", "Utiliser |RA| pour déplacer la caméra"},
    {"Appuyer sur |D-PAD-LR| pour changer d'arme", "Utiliser |D-PAD-LR| pour changer d'arme"},
    // Spanish (two banks: the second one ends its sentences with a full stop)
    {"Mantén pulsado |X| o |R2| para acelerar", "Mantén pulsado |X| para acelerar"},
    {"Mantén pulsado |X| o |R2| para acelerar.", "Mantén pulsado |X| para acelerar."},
    {"Pulsa |C| o |S| para frenar y dar marcha atrás", "Pulsa |S| para frenar y dar marcha atrás"},
    {"Mantén pulsado |C| o |S| para frenar y dar marcha atrás.", "Mantén pulsado |S| para frenar y dar marcha atrás."},
    {"Haz clic en |A| izquierdo para cambiar de hombro", "Pulsa |L3| para cambiar de hombro"},
    {"Pulsa |A| izquierdo para cambiar de hombro.", "Pulsa |L3| para cambiar de hombro."},
    {"Haz clic en |A| derecho para apuntar la cámara en la dirección que mira Drake",
     "Pulsa |R3| para apuntar la cámara en la dirección que mira Drake"},
    {"Pulsa |A| derecho para dirigir la cámara hacia donde mira_Drake.", "Pulsa |R3| para dirigir la cámara hacia donde mira_Drake."},
    {"Usa |A| derecho para mover la cámara", "Usa |RA| para mover la cámara"},
    {"Usa |A| izquierdo para mover a Drake", "Usa |LA| para mover a Drake"},
    {"Use |A| izquierdo para mover a Drake.", "Usa |LA| para mover a Drake."},
    {"Pulsa |D-PAD-LR| para cambiar de arma", "Usa |D-PAD-LR| para cambiar de arma"},
    {"Pulsa |D-PAD-LR| para cambiar de arma.", "Usa |D-PAD-LR| para cambiar de arma."},
    // Portuguese (Brazil, then Portugal)
    {"Segure |X| ou |R2| para acelerar", "Segure |X| para acelerar"},
    {"Segure |C| ou |S| para frear e dar ré", "Segure |S| para frear e dar ré"},
    {"Clique em |A| à esquerda para trocar de ombros", "Pressione |L3| para trocar de ombros"},
    {"Use |A| à direita para movimentar a câmera", "Use |RA| para movimentar a câmera"},
    {"Use |A| à esquerda para movimentar Drake", "Use |LA| para movimentar Drake"},
    {"Clique em |A| à direita para apontar a câmera para onde Drake estiver olhando",
     "Pressione |R3| para apontar a câmera para onde Drake estiver olhando"},
    {"Pressione |D-PAD-LR| para trocar as armas", "Use |D-PAD-LR| para trocar as armas"},
    {"Mantém |X| ou |R2| premido para acelerares", "Mantém |X| premido para acelerares"},
    {"Mantém |C| ou |S| premido para travares e engatares a marcha atrás", "Mantém |S| premido para travares e engatares a marcha atrás"},
    {"Prime o |A| esquerdo para trocares de ombro", "Prime |L3| para trocares de ombro"},
    {"Utiliza o |A| direito para movimentares a câmara", "Utiliza |RA| para movimentares a câmara"},
    {"Utiliza o |A| esquerdo para movimentares o Drake", "Utiliza |LA| para movimentares o Drake"},
    {"Utiliza o |A| direito para apontares a câmara para onde o Drake está a_olhar",
     "Prime |R3| para apontares a câmara para onde o Drake está a_olhar"},
    {"Prime |D-PAD-LR| para mudares de arma", "Usa |D-PAD-LR| para mudares de arma"},
};

struct Prepared {
    std::string original, replacement; // the text searched for, and what is written over it
    size_t bar; // offset of the first '|' in the original
};

/// forward: the game's text becomes the reworded one (keyboard and mouse); else the reworded one (padded as it is in memory)
/// becomes the game's text again (a controller is in use).
std::vector<Prepared> Prepare(bool forward) {
    std::vector<Prepared> out;
    for (const Rewording& r : Rewordings) {
        Prepared p{r.original, r.replacement, 0};
        if (p.replacement.size() > p.original.size() || p.original.find('|') == std::string::npos) {
            continue; // a mistake in the table: this one is left alone
        }
        p.replacement.append(p.original.size() - p.replacement.size(), ' ');
        if (!forward) {
            std::swap(p.original, p.replacement);
        }
        p.bar = p.original.find('|');
        if (p.bar == std::string::npos) {
            continue;
        }
        out.push_back(std::move(p));
    }
    return out;
}

// The guest memory of the port lives in this window; the host mappings of the GPU (slow to read) do not.
constexpr uintptr_t ScanFrom = 0x1000000000ull;
constexpr uintptr_t ScanTo = 0x1400000000ull;

std::atomic<int64_t> g_request{0}; // steady-clock milliseconds of the last request
std::atomic<bool> g_keyboard{true}; // the reworded hints are wanted (keyboard and mouse), not the game's texts (controller)
std::atomic<bool> g_xbox{false};    // an Xbox controller: the shoulder names of the Controls menu are LB / RB and LT / RT
std::mutex g_seen_lock;
std::vector<uintptr_t> g_seen;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// Rewords every copy of the texts in the guest memory; returns how many were changed.
int PatchAll(const std::vector<Prepared>& texts) {
    size_t max_len = 1;
    for (const Prepared& p : texts) {
        max_len = std::max(max_len, p.original.size());
    }
    const HANDLE self = GetCurrentProcess();
    std::vector<char> buf(4u << 20);
    int patched = 0;
    uintptr_t addr = ScanFrom;
    while (addr < ScanTo) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
            break;
        }
        const uintptr_t base = uintptr_t(mbi.BaseAddress), size = mbi.RegionSize;
        addr = base + size;
        const DWORD prot = mbi.Protect;
        const bool readable = mbi.State == MEM_COMMIT && !(prot & (PAGE_GUARD | PAGE_NOACCESS)) &&
                              (prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
        if (!readable) {
            continue;
        }
        for (uintptr_t off = 0; off < size;) {
            const size_t n = std::min<size_t>(buf.size(), size - off);
            SIZE_T got = 0;
            if (!ReadProcessMemory(self, reinterpret_cast<void*>(base + off), buf.data(), n, &got) || got == 0) {
                break;
            }
            for (size_t i = 0; i < got;) {
                const void* bar = std::memchr(buf.data() + i, '|', got - i);
                if (!bar) {
                    break;
                }
                const size_t at = size_t(static_cast<const char*>(bar) - buf.data());
                i = at + 1;
                for (const Prepared& p : texts) {
                    if (at < p.bar || at - p.bar + p.original.size() > got ||
                        std::memcmp(buf.data() + at - p.bar, p.original.data(), p.original.size()) != 0) {
                        continue;
                    }
                    const size_t end = at - p.bar + p.original.size();
                    if (end < got && buf[end] == '.' && p.original.back() != '.') {
                        continue; // the same sentence with a full stop (another Spanish bank) has its own entry
                    }
                    const uintptr_t where = base + off + at - p.bar;
                    SIZE_T written = 0;
                    if (!WriteProcessMemory(self, reinterpret_cast<void*>(where), p.replacement.data(), p.replacement.size(),
                                            &written) ||
                        written != p.replacement.size()) {
                        continue;
                    }
                    ++patched;
                    std::lock_guard<std::mutex> lock(g_seen_lock);
                    if (std::find(g_seen.begin(), g_seen.end(), where) == g_seen.end()) {
                        g_seen.push_back(where);
                        std::printf("KeyIcons: hint text reworded at 0x%llx (\"%s\")\n", static_cast<unsigned long long>(where),
                                    p.replacement.c_str());
                    }
                }
            }
            // A text cut by the end of the chunk is read whole with the next one.
            off += n > max_len ? n - (max_len - 1) : n;
            if (n < buf.size()) {
                break;
            }
        }
    }
    return patched;
}

/// Where the shoulder names of the menus were found: the L of "L1 / R1" and the R (their addresses in the guest's memory). Once known, a
/// change of device writes them at once (a few microseconds), with no search of the memory.
struct NameSpot {
    uintptr_t l, r;
};
std::mutex g_spots_lock;
std::vector<NameSpot> g_spots;

/// Whether a spot is still a button name (the memory may have been given to something else since): L, a digit or a letter, ..., R, the
/// same digit or letter, the end of the string.
bool SpotIsValid(const NameSpot& spot, char& current) {
    const HANDLE self = GetCurrentProcess();
    char before_l = 0, at_l = 0, before_r = 0, at_r = 0, after_r = 1;
    SIZE_T got = 0;
    if (!ReadProcessMemory(self, reinterpret_cast<void*>(spot.l - 1), &before_l, 1, &got) || got != 1 ||
        !ReadProcessMemory(self, reinterpret_cast<void*>(spot.l), &at_l, 1, &got) || got != 1 ||
        !ReadProcessMemory(self, reinterpret_cast<void*>(spot.r - 1), &before_r, 1, &got) || got != 1 ||
        !ReadProcessMemory(self, reinterpret_cast<void*>(spot.r), &at_r, 1, &got) || got != 1 ||
        !ReadProcessMemory(self, reinterpret_cast<void*>(spot.r + 1), &after_r, 1, &got) || got != 1) {
        return false;
    }
    current = at_l;
    return before_l == 'L' && before_r == 'R' && at_l == at_r && after_r == 0 && (at_l == '1' || at_l == '2' || at_l == 'B' || at_l == 'T');
}

/// Writes the names of every known spot for the device: L1 / R1 and L2 / R2 (PlayStation pad, keyboard), LB / RB and LT / RT (Xbox).
int ApplyNameSpots(bool xbox) {
    std::lock_guard<std::mutex> lock(g_spots_lock);
    const HANDLE self = GetCurrentProcess();
    int written_count = 0;
    for (size_t i = 0; i < g_spots.size();) {
        char current = 0;
        if (!SpotIsValid(g_spots[i], current)) {
            g_spots.erase(g_spots.begin() + std::ptrdiff_t(i)); // the text bank went away
            continue;
        }
        const char target = xbox ? (current == '1' || current == 'B' ? 'B' : 'T') : (current == 'B' || current == '1' ? '1' : '2');
        if (target != current) {
            SIZE_T written = 0;
            if (WriteProcessMemory(self, reinterpret_cast<void*>(g_spots[i].l), &target, 1, &written) && written == 1 &&
                WriteProcessMemory(self, reinterpret_cast<void*>(g_spots[i].r), &target, 1, &written) && written == 1) {
                ++written_count;
            }
        }
        ++i;
    }
    return written_count;
}

/// The Controls menu names the shoulder buttons as plain texts: "L1 / R1" and "L2 / R2" (and, in the other menus, "L1 and R1", "L2 et R2"
/// and so on in each language): a string that is nothing but L1 or L2, a short separator and R1 or R2. For an Xbox controller they become
/// LB / RB and LT / RT (same length: written in place); for any other device they are put back. The strings are searched whole, between
/// two NUL bytes, so no other text can be taken for them. Every one found is remembered (g_spots), so that the next change of device does
/// not need another search.
int PatchButtonNames(bool xbox) {
    static const struct {
        char number, letter;
    } pairs[] = {{'1', 'B'}, {'2', 'T'}};
    const HANDLE self = GetCurrentProcess();
    constexpr size_t Overlap = 24;
    std::vector<char> buf(4u << 20);
    int patched = 0;
    uintptr_t addr = ScanFrom;
    while (addr < ScanTo) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
            break;
        }
        const uintptr_t base = uintptr_t(mbi.BaseAddress), size = mbi.RegionSize;
        addr = base + size;
        const DWORD prot = mbi.Protect;
        const bool readable = mbi.State == MEM_COMMIT && !(prot & (PAGE_GUARD | PAGE_NOACCESS)) &&
                              (prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
        if (!readable) {
            continue;
        }
        for (uintptr_t off = 0; off < size;) {
            const size_t n = std::min<size_t>(buf.size(), size - off);
            SIZE_T got = 0;
            if (!ReadProcessMemory(self, reinterpret_cast<void*>(base + off), buf.data(), n, &got) || got == 0) {
                break;
            }
            for (size_t i = 0; i + 8 < got; ++i) {
                if (buf[i] != 0 || buf[i + 1] != 'L') {
                    continue;
                }
                for (const auto& pr : pairs) {
                    const char current = buf[i + 2];
                    if (current != pr.number && current != pr.letter) {
                        continue;
                    }
                    const char target = xbox ? pr.letter : pr.number;
                    for (size_t k = 3; k <= 11 && i + k + 3 < got; ++k) { // "R", the same character, the end of the string
                        if (buf[i + k] == 0) {
                            break;
                        }
                        if (buf[i + k] == 'R' && buf[i + k + 1] == current && buf[i + k + 2] == 0 && k >= 4) {
                            const uintptr_t where_l = base + off + i + 2, where_r = base + off + i + k + 1;
                            {
                                std::lock_guard<std::mutex> lock(g_spots_lock);
                                const bool known = std::any_of(g_spots.begin(), g_spots.end(), [&](const NameSpot& sp) { return sp.l == where_l; });
                                if (!known) {
                                    g_spots.push_back({where_l, where_r});
                                    std::printf("KeyIcons: button names text at 0x%llx noted\n", static_cast<unsigned long long>(where_l - 2));
                                }
                            }
                            if (current != target) {
                                SIZE_T written = 0;
                                if (WriteProcessMemory(self, reinterpret_cast<void*>(where_l), &target, 1, &written) && written == 1 &&
                                    WriteProcessMemory(self, reinterpret_cast<void*>(where_r), &target, 1, &written) && written == 1) {
                                    ++patched;
                                }
                            }
                            break;
                        }
                    }
                }
            }
            off += n > Overlap ? n - (Overlap - 1) : n;
            if (n < buf.size()) {
                break;
            }
        }
    }
    return patched;
}

/// After each request (the icon atlas was just uploaded: the game is loading), looks for the texts 2 s
/// and 10 s later, since the text bank may come before or after.
void Worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const std::vector<Prepared> reworded = Prepare(true), restored = Prepare(false);
    int64_t handled = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const int64_t request = g_request.load();
        if (request == handled) {
            continue;
        }
        handled = request;
        for (const int delay : {2000, 8000}) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            PatchAll(g_keyboard ? reworded : restored);
            PatchButtonNames(g_xbox);
        }
    }
}

} // namespace

void RequestHintPatch() {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(Worker).detach(); });
    g_request.store(NowMs());
}

void SetHintsMode(int mode) {
    g_keyboard = mode == 0;
    g_xbox = mode == 2;
    const int now = ApplyNameSpots(mode == 2); // the names already found change at once, as the pastilles do
    if (now > 0) {
        std::printf("KeyIcons: %d button names text(s) written at once\n", now);
    }
    RequestHintPatch(); // (the search runs on for the hints and for names not found yet)
}

#else

void RequestHintPatch() {}
void SetHintsMode(int) {}

#endif

} // namespace BbKeyIcons
