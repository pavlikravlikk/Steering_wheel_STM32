import glob
import math
import os
import random
import struct
import sys

import pygame

try:
    import hid
except ImportError:
    hid = None

# ====================== SETTINGS ======================
VID, PID = 0x0483, 0x5752    # STM32 Steering Wheel Controller
REPORT_FMT = "<HHHH"         # X, Y, Z (uint16) + 16 button bits
REPORT_LEN = struct.calcsize(REPORT_FMT)   # 8 bytes (default build, no Report ID)
JOYSTICK_REPORT_ID = 0x01    # FFB build: Report ID 0x01 + the same 8 bytes
AXIS_MAX = 65535
AXIS_CENTER = AXIS_MAX / 2   # raw X value with the wheel straight
NUM_BUTTONS = 16             # HID buttons 1..16 (1..10 wired: 8 buttons + 2 paddles)
WIRED_BUTTONS = 10
MAX_ANGLE = 270.0            # display angle per side (540 deg lock-to-lock)
INVERT = False               # True if the wheel turns the wrong way
RESPONSE = 14.0              # smoothing speed (higher = snappier)

W, H = 1000, 680
WHEEL_R = 190
WHEEL_C = (320, 340)
PANEL = pygame.Rect(650, 40, 310, 600)
FPS = 60

# Demo mode: keys 1..9 and 0 emulate buttons 1..10.
DEMO_BUTTON_KEYS = [pygame.K_1, pygame.K_2, pygame.K_3, pygame.K_4, pygame.K_5,
                    pygame.K_6, pygame.K_7, pygame.K_8, pygame.K_9, pygame.K_0]


# ====================== HID CONNECTION ======================
class HidrawDevice:
    """Read directly from /dev/hidrawN (Linux), without the hidapi library."""

    def __init__(self, path):
        self.fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)

    def read(self, size):
        try:
            return list(os.read(self.fd, size))
        except BlockingIOError:
            return []
        except OSError as e:
            raise OSError(e)


def open_hidraw_linux():
    """Find the STM32 (VID 0483, PID 5752) among /dev/hidraw*."""
    for uevent in glob.glob("/sys/class/hidraw/hidraw*/device/uevent"):
        try:
            text = open(uevent).read().upper()
        except OSError:
            continue
        if "0483" in text and "5752" in text:
            node = uevent.split("/")[4]
            return HidrawDevice("/dev/" + node), "STM32 Steering Wheel (" + node + ")"
    return None, None


def open_gamepad():
    if sys.platform.startswith("linux"):
        try:
            dev, name = open_hidraw_linux()
            if dev:
                return dev, name
        except OSError as e:
            print("[HIDRAW]", e, "- a udev rule or sudo is required (see README)")
    if hid is None:
        return None, "Demo mode (hid not installed)"
    try:
        for info in hid.enumerate():
            name = info.get("product_string", "") or ""
            if (info.get("vendor_id") == VID and info.get("product_id") == PID) \
                    or "STEERING WHEEL" in name.upper():
                dev = hid.device()
                dev.open_path(info["path"])
                dev.set_nonblocking(True)
                return dev, name
    except Exception as e:
        print("[HID]", e)
    return None, "Demo mode (arrows / A D, keys 1-0 = buttons)"


def parse_report(data):
    """Return (x, y, z, buttons) or None. Accepts the 8-byte report of the
    default build and the 9-byte report (Report ID 0x01) of the FFB build;
    other FFB reports (PID state etc.) are ignored."""
    data = bytes(data)
    if len(data) == REPORT_LEN:
        return struct.unpack_from(REPORT_FMT, data)
    if len(data) >= REPORT_LEN + 1 and data[0] == JOYSTICK_REPORT_ID:
        return struct.unpack_from(REPORT_FMT, data, 1)
    return None


# ====================== GRAPHICS ======================
def lerp(a, b, t):
    return a + (b - a) * t


def lerp_color(c1, c2, t):
    return tuple(int(lerp(a, b, t)) for a, b in zip(c1, c2))


def make_background():
    bg = pygame.Surface((W, H))
    top, bottom = (10, 12, 24), (26, 18, 48)
    for y in range(H):
        pygame.draw.line(bg, lerp_color(top, bottom, y / H), (0, y), (W, y))
    # tiny "stars"
    rnd = random.Random(7)
    for _ in range(90):
        x, y = rnd.randrange(W), rnd.randrange(H)
        b = rnd.randrange(50, 130)
        bg.set_at((x, y), (b, b, min(255, b + 40)))
    return bg


def make_glow(radius, color):
    surf = pygame.Surface((radius * 2, radius * 2), pygame.SRCALPHA)
    for r in range(radius, 0, -3):
        a = int(70 * (1 - r / radius) ** 2)
        pygame.draw.circle(surf, (*color, a), (radius, radius), r)
    return surf


def make_wheel(R):
    ss = 3                                   # supersampling for smooth edges
    S = R * 2 * ss
    surf = pygame.Surface((S, S), pygame.SRCALPHA)
    c = S // 2
    r = R * ss
    t = 30 * ss                              # rim thickness
    inner = r - t // 2 + 2 * ss              # where the spokes end

    # --- spokes (shadow + metal) ---
    spokes = [
        ((c, c + 10 * ss), (c, c + inner)),
        ((c - 20 * ss, c - 6 * ss), (c - inner * 0.97, c - 36 * ss)),
        ((c + 20 * ss, c - 6 * ss), (c + inner * 0.97, c - 36 * ss)),
    ]
    for a, b in spokes:
        pygame.draw.line(surf, (40, 42, 52), a, b, 30 * ss)
    for a, b in spokes:
        pygame.draw.line(surf, (150, 153, 165), a, b, 24 * ss)
        pygame.draw.line(surf, (215, 218, 228), a, b, 8 * ss)

    # --- rim ---
    pygame.draw.circle(surf, (12, 14, 22), (c, c), r, t)                    # outline
    pygame.draw.circle(surf, (28, 78, 205), (c, c), r - 3 * ss, t - 6 * ss)  # leather
    ring_rect = pygame.Rect(0, 0, 2 * (r - t // 2), 2 * (r - t // 2))
    ring_rect.center = (c, c)
    # top-left highlight and bottom-right shadow
    pygame.draw.arc(surf, (110, 165, 255), ring_rect, math.radians(95), math.radians(175), 7 * ss)
    pygame.draw.arc(surf, (14, 40, 120), ring_rect, math.radians(280), math.radians(355), 8 * ss)

    # --- stitching ---
    rs = r - t // 2
    for i in range(120):
        ang = i * math.tau / 120
        x = c + math.cos(ang) * rs
        y = c + math.sin(ang) * rs
        pygame.draw.circle(surf, (225, 232, 250), (x, y), max(1, int(1.6 * ss)))

    # --- 12 o'clock marker ---
    mk = pygame.Rect(0, 0, 9 * ss, t - 6 * ss)
    mk.center = (c, c - rs)
    pygame.draw.rect(surf, (255, 50, 50), mk, border_radius=3 * ss)

    # --- centre hub with gradient ---
    hub = 62 * ss
    for i in range(hub, 0, -1):
        col = lerp_color((235, 238, 248), (85, 88, 102), i / hub)
        pygame.draw.circle(surf, col, (c - (hub - i) * 0.12, c - (hub - i) * 0.12), i)
    pygame.draw.circle(surf, (20, 22, 32), (c, c), hub, 3 * ss)
    pygame.draw.circle(surf, (28, 78, 205), (c, c), 36 * ss)
    pygame.draw.circle(surf, (230, 235, 250), (c, c), 36 * ss, 3 * ss)
    f = pygame.font.SysFont("segoeui,arial,calibri", 46 * ss, bold=True)
    letter = f.render("B", True, (255, 255, 255))
    surf.blit(letter, letter.get_rect(center=(c, c + 2 * ss)))

    return pygame.transform.smoothscale(surf, (R * 2, R * 2))


def draw_round_bar(screen, rect, value, color_pos, color_neg, bg=(30, 32, 48)):
    """Centred bar: value from -1 to +1."""
    pygame.draw.rect(screen, bg, rect, border_radius=rect.height // 2)
    cx = rect.centerx
    half = rect.width // 2
    w = int(abs(value) * half)
    if w > 0:
        r2 = pygame.Rect(cx, rect.y, w, rect.height) if value > 0 else pygame.Rect(cx - w, rect.y, w, rect.height)
        pygame.draw.rect(screen, color_pos if value > 0 else color_neg, r2, border_radius=rect.height // 2)
    pygame.draw.line(screen, (230, 230, 240), (cx, rect.y - 3), (cx, rect.bottom + 3), 2)


def draw_text(screen, font, text, pos, color=(235, 238, 250), anchor="topleft"):
    img = font.render(text, True, color)
    r = img.get_rect()
    setattr(r, anchor, pos)
    screen.blit(img, r)


# ====================== GAME ======================
class Game:
    def __init__(self):
        self.reset()
        self.car = self.make_car()

    def reset(self):
        self.offset = 0.0
        self.obstacles = []
        self.timer = 0.0
        self.score = 0
        self.lives = 3
        self.flash = 0.0
        self.speed = 260.0
        self.over = False

    @staticmethod
    def make_car():
        s = pygame.Surface((44, 82), pygame.SRCALPHA)
        pygame.draw.rect(s, (0, 0, 0, 90), (3, 6, 40, 78), border_radius=12)           # shadow
        for x, y in ((0, 12), (36, 12), (0, 54), (36, 54)):
            pygame.draw.rect(s, (15, 15, 20), (x, y, 8, 18), border_radius=3)           # wheels
        pygame.draw.rect(s, (225, 40, 55), (5, 2, 34, 76), border_radius=14)            # body
        pygame.draw.rect(s, (255, 110, 120), (9, 4, 12, 70), border_radius=8)           # highlight
        pygame.draw.rect(s, (25, 30, 55), (10, 22, 24, 16), border_radius=5)            # windscreen
        pygame.draw.rect(s, (25, 30, 55), (11, 52, 22, 10), border_radius=5)            # rear window
        pygame.draw.circle(s, (255, 245, 180), (12, 6), 3)                              # headlights
        pygame.draw.circle(s, (255, 245, 180), (32, 6), 3)
        return s

    def update(self, dt, steer):
        self.flash = max(0.0, self.flash - dt)
        if self.over:
            return
        self.offset = (self.offset + self.speed * dt) % 60
        self.speed = min(520.0, self.speed + 4 * dt)

        self.timer -= dt
        if self.timer <= 0:
            self.obstacles.append({"x": random.uniform(-0.85, 0.85), "y": -40.0})
            self.timer = random.uniform(0.7, 1.3) * (260 / self.speed) * 1.4

        car_rect = self.car_rect(steer)
        for o in self.obstacles[:]:
            o["y"] += self.speed * dt
            orect = self.obstacle_rect(o)
            if orect.colliderect(car_rect.inflate(-10, -14)):
                self.obstacles.remove(o)
                self.lives -= 1
                self.flash = 0.35
                if self.lives <= 0:
                    self.over = True
            elif o["y"] > PANEL.bottom + 40:
                self.obstacles.remove(o)
                self.score += 1

    def road_rect(self):
        return pygame.Rect(PANEL.x + 28, PANEL.y, PANEL.width - 56, PANEL.height)

    def car_rect(self, steer):
        road = self.road_rect()
        span = road.width / 2 - 36
        x = road.centerx + steer * span
        return pygame.Rect(0, 0, 44, 82).move(int(x - 22), PANEL.bottom - 130)

    def obstacle_rect(self, o):
        road = self.road_rect()
        span = road.width / 2 - 28
        x = road.centerx + o["x"] * span
        return pygame.Rect(int(x - 14), int(o["y"] - 14), 28, 28)

    def draw(self, screen, steer, fonts):
        # panel frame
        pygame.draw.rect(screen, (14, 16, 28), PANEL.inflate(10, 10), border_radius=22)
        old_clip = screen.get_clip()
        screen.set_clip(PANEL)
        pygame.draw.rect(screen, (22, 128, 70), PANEL)                          # grass
        road = self.road_rect()
        pygame.draw.rect(screen, (48, 50, 60), road)                            # asphalt
        pygame.draw.rect(screen, (240, 240, 245), (road.x, PANEL.y, 5, PANEL.height))
        pygame.draw.rect(screen, (240, 240, 245), (road.right - 5, PANEL.y, 5, PANEL.height))
        for y in range(PANEL.y - 60, PANEL.bottom + 60, 60):                    # lane markings
            pygame.draw.rect(screen, (250, 210, 70), (road.centerx - 3, y + self.offset, 6, 32), border_radius=3)

        for o in self.obstacles:                                               # cones
            r = self.obstacle_rect(o)
            pygame.draw.ellipse(screen, (0, 0, 0), r.inflate(6, -10).move(2, 10))
            pygame.draw.polygon(screen, (255, 120, 20), [(r.centerx, r.y), (r.right - 2, r.bottom - 4), (r.x + 2, r.bottom - 4)])
            pygame.draw.polygon(screen, (255, 255, 255), [(r.centerx - 6, r.y + 12), (r.centerx + 6, r.y + 12),
                                                          (r.centerx + 9, r.y + 18), (r.centerx - 9, r.y + 18)])
            pygame.draw.rect(screen, (30, 30, 35), (r.x, r.bottom - 5, r.width, 5), border_radius=2)

        cr = self.car_rect(steer)                                              # car
        tilted = pygame.transform.rotozoom(self.car, -steer * 14, 1.0)
        screen.blit(tilted, tilted.get_rect(center=cr.center))

        if self.flash > 0:                                                     # red flash on impact
            fl = pygame.Surface(PANEL.size, pygame.SRCALPHA)
            fl.fill((255, 40, 40, int(150 * self.flash / 0.35)))
            screen.blit(fl, PANEL.topleft)
        screen.set_clip(old_clip)

        # HUD
        draw_text(screen, fonts["small"], f"Score: {self.score}", (PANEL.x + 14, PANEL.y + 10), (255, 255, 255))
        for i in range(3):
            col = (255, 70, 90) if i < self.lives else (70, 70, 85)
            pygame.draw.circle(screen, col, (PANEL.right - 24 - i * 24, PANEL.y + 22), 8)

        if self.over:
            ov = pygame.Surface(PANEL.size, pygame.SRCALPHA)
            ov.fill((0, 0, 0, 160))
            screen.blit(ov, PANEL.topleft)
            draw_text(screen, fonts["big"], "GAME OVER", PANEL.center, (255, 255, 255), "center")
            draw_text(screen, fonts["small"], f"Score: {self.score}", (PANEL.centerx, PANEL.centery + 40), (255, 220, 120), "center")
            draw_text(screen, fonts["small"], "R / Space - restart", (PANEL.centerx, PANEL.centery + 72), (190, 195, 215), "center")


# ====================== MAIN LOOP ======================
def main():
    gamepad, device_name = open_gamepad()
    demo = gamepad is None

    pygame.init()
    screen = pygame.display.set_mode((W, H))
    pygame.display.set_caption("Steering Wheel")
    clock = pygame.time.Clock()

    fonts = {
        "title": pygame.font.SysFont("segoeui,arial,calibri", 30, bold=True),
        "big": pygame.font.SysFont("segoeui,arial,calibri", 30, bold=True),
        "small": pygame.font.SysFont("segoeui,arial,calibri", 22, bold=True),
        "tiny": pygame.font.SysFont("segoeui,arial,calibri", 17),
        "micro": pygame.font.SysFont("segoeui,arial,calibri", 13, bold=True),
    }

    background = make_background()
    wheel_img = make_wheel(WHEEL_R)
    glow = make_glow(300, (50, 110, 255))
    shadow = pygame.Surface((WHEEL_R * 2 + 80, WHEEL_R * 2 + 80), pygame.SRCALPHA)
    pygame.draw.ellipse(shadow, (0, 0, 0, 90), shadow.get_rect().inflate(-40, -40))

    game = Game()
    x_raw = int(AXIS_CENTER)
    y_raw = z_raw = 0
    buttons = 0
    axis_val = 0.0
    smoothed = 0.0
    peak = 0.0

    running = True
    try:
        while running:
            dt = clock.tick(FPS) / 1000.0
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    running = False
                elif event.type == pygame.KEYDOWN:
                    if event.key == pygame.K_ESCAPE:
                        running = False
                    elif event.key in (pygame.K_r, pygame.K_SPACE):
                        game.reset()

            # --- read the wheel ---
            if demo:
                keys = pygame.key.get_pressed()
                target = (keys[pygame.K_RIGHT] or keys[pygame.K_d]) - (keys[pygame.K_LEFT] or keys[pygame.K_a])
                axis_val += max(-3 * dt, min(3 * dt, target - axis_val))
                x_raw = int(AXIS_CENTER + axis_val * AXIS_CENTER)
                buttons = 0
                for i, key in enumerate(DEMO_BUTTON_KEYS):
                    if keys[key]:
                        buttons |= 1 << i
            else:
                latest = None
                try:
                    while True:
                        rep = gamepad.read(64)
                        if not rep:
                            break
                        parsed = parse_report(rep)
                        if parsed is not None:
                            latest = parsed
                except OSError:
                    demo, device_name = True, "Device disconnected"
                if latest:
                    x_raw, y_raw, z_raw, buttons = latest
                    axis_val = max(-1.0, min(1.0, (x_raw - AXIS_CENTER) / AXIS_CENTER))

            if INVERT:
                axis_val_used = -axis_val
            else:
                axis_val_used = axis_val
            smoothed += (axis_val_used - smoothed) * min(1.0, dt * RESPONSE)
            angle = smoothed * MAX_ANGLE
            peak = max(peak * (1 - dt * 0.4), abs(smoothed))

            game.update(dt, smoothed)

            # --- drawing ---
            screen.blit(background, (0, 0))

            intensity = min(1.0, abs(smoothed))
            g = glow.copy()
            g.set_alpha(int(110 + 145 * intensity))
            screen.blit(g, g.get_rect(center=WHEEL_C))
            screen.blit(shadow, shadow.get_rect(center=(WHEEL_C[0] + 8, WHEEL_C[1] + 22)))

            rotated = pygame.transform.rotozoom(wheel_img, -angle, 1.0)
            screen.blit(rotated, rotated.get_rect(center=WHEEL_C))

            # header
            draw_text(screen, fonts["title"], "Steering Wheel", (30, 24))
            dot = (80, 230, 130) if not demo else (255, 190, 60)
            pygame.draw.circle(screen, dot, (36, 74), 6)
            draw_text(screen, fonts["tiny"], device_name, (50, 64), (170, 178, 205))

            # indicators below the wheel
            bar = pygame.Rect(WHEEL_C[0] - 200, 600, 400, 14)
            draw_round_bar(screen, bar, smoothed, (80, 220, 255), (255, 170, 80))
            draw_text(screen, fonts["small"], f"{angle:+.1f}°", (WHEEL_C[0], 575), (200, 255, 120), "center")
            draw_text(screen, fonts["tiny"], f"X {x_raw:05d}  Y {y_raw:05d}  Z {z_raw:05d}", (bar.x, 628), (150, 160, 190))
            draw_text(screen, fonts["tiny"], f"Axis {smoothed:+.2f}", (bar.right, 628), (150, 160, 190), "topright")

            # reserved pedal axes Y (throttle) and Z (brake): 0..65535
            for row, (label, raw, col) in enumerate((("Y throttle", y_raw, (90, 230, 120)),
                                                     ("Z brake", z_raw, (255, 90, 90)))):
                by = 100 + row * 26
                pygame.draw.rect(screen, (30, 32, 48), (120, by, 150, 12), border_radius=6)
                wbar = int(150 * max(0, min(AXIS_MAX, raw)) / AXIS_MAX)
                if wbar > 0:
                    pygame.draw.rect(screen, col, (120, by, wbar, 12), border_radius=6)
                draw_text(screen, fonts["micro"], label, (30, by - 1), (150, 160, 190))

            # buttons 1..16 (lit = pressed; 1..10 are wired, 11..16 dimmed)
            for i in range(NUM_BUTTONS):
                bx = WHEEL_C[0] + int((i - (NUM_BUTTONS - 1) / 2) * 29)
                on = bool(buttons & (1 << i))
                idle = (38, 42, 62) if i < WIRED_BUTTONS else (26, 28, 40)
                pygame.draw.circle(screen, (255, 210, 70) if on else idle, (bx, 662), 12)
                pygame.draw.circle(screen, (230, 235, 250) if on else (80, 88, 120), (bx, 662), 12, 2)
                draw_text(screen, fonts["micro"], str(i + 1), (bx, 662),
                          (20, 22, 32) if on else (150, 160, 190), "center")

            # peak marker on the bar
            px = bar.centerx + int(math.copysign(peak, smoothed) * bar.width / 2)
            pygame.draw.circle(screen, (255, 255, 255), (px, bar.centery), 4)

            game.draw(screen, smoothed, fonts)
            draw_text(screen, fonts["tiny"], "ESC - quit  |  R - restart", (PANEL.centerx, H - 22), (120, 128, 160), "center")

            pygame.display.flip()
    finally:
        if gamepad is not None:
            try:
                gamepad.close()
            except Exception:
                pass
        pygame.quit()
        sys.exit()


if __name__ == "__main__":
    main()