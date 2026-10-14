import math
import random
import sys

import pygame

try:
    import hid
except ImportError:
    hid = None

# ====================== НАЛАШТУВАННЯ ======================
ADC_MAX = 4095
ADC_CENTER = ADC_MAX / 2     # значення АЦП у прямому положенні керма
MAX_ANGLE = 180.0            # кут повороту в кожен бік
INVERT = False               # True, якщо кермо крутиться в протилежний бік
RESPONSE = 14.0              # швидкість згладжування (більше = різкіше)

W, H = 1000, 680
WHEEL_R = 190
WHEEL_C = (320, 340)
PANEL = pygame.Rect(650, 40, 310, 600)
FPS = 60


# ====================== ПІДКЛЮЧЕННЯ HID ======================
def open_gamepad():
    if hid is None:
        return None, "Демо-режим (немає hid)"
    try:
        for info in hid.enumerate():
            name = info.get("product_string", "") or ""
            if "STEERING WHEEL" in name.upper():
                dev = hid.device()
                dev.open_path(info["path"])
                dev.set_nonblocking(True)
                return dev, name
    except Exception as e:
        print("[HID]", e)
    return None, "Демо-режим (стрілки / A D)"


# ====================== ГРАФІКА ======================
def lerp(a, b, t):
    return a + (b - a) * t


def lerp_color(c1, c2, t):
    return tuple(int(lerp(a, b, t)) for a, b in zip(c1, c2))


def make_background():
    bg = pygame.Surface((W, H))
    top, bottom = (10, 12, 24), (26, 18, 48)
    for y in range(H):
        pygame.draw.line(bg, lerp_color(top, bottom, y / H), (0, y), (W, y))
    # дрібні «зорі»
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
    ss = 3                                   # суперсемплінг для плавних країв
    S = R * 2 * ss
    surf = pygame.Surface((S, S), pygame.SRCALPHA)
    c = S // 2
    r = R * ss
    t = 30 * ss                              # товщина обода
    inner = r - t // 2 + 2 * ss              # куди впираються спиці

    # --- спиці (тінь + метал) ---
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

    # --- обід ---
    pygame.draw.circle(surf, (12, 14, 22), (c, c), r, t)                    # контур
    pygame.draw.circle(surf, (28, 78, 205), (c, c), r - 3 * ss, t - 6 * ss)  # шкіра
    ring_rect = pygame.Rect(0, 0, 2 * (r - t // 2), 2 * (r - t // 2))
    ring_rect.center = (c, c)
    # блік зверху-ліворуч та тінь знизу-праворуч
    pygame.draw.arc(surf, (110, 165, 255), ring_rect, math.radians(95), math.radians(175), 7 * ss)
    pygame.draw.arc(surf, (14, 40, 120), ring_rect, math.radians(280), math.radians(355), 8 * ss)

    # --- строчка ---
    rs = r - t // 2
    for i in range(120):
        ang = i * math.tau / 120
        x = c + math.cos(ang) * rs
        y = c + math.sin(ang) * rs
        pygame.draw.circle(surf, (225, 232, 250), (x, y), max(1, int(1.6 * ss)))

    # --- маркер «12 годин» ---
    mk = pygame.Rect(0, 0, 9 * ss, t - 6 * ss)
    mk.center = (c, c - rs)
    pygame.draw.rect(surf, (255, 50, 50), mk, border_radius=3 * ss)

    # --- центральний хаб з градієнтом ---
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
    """Центрована смуга: value від -1 до +1."""
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


# ====================== ГРА ======================
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
        pygame.draw.rect(s, (0, 0, 0, 90), (3, 6, 40, 78), border_radius=12)           # тінь
        for x, y in ((0, 12), (36, 12), (0, 54), (36, 54)):
            pygame.draw.rect(s, (15, 15, 20), (x, y, 8, 18), border_radius=3)           # колеса
        pygame.draw.rect(s, (225, 40, 55), (5, 2, 34, 76), border_radius=14)            # кузов
        pygame.draw.rect(s, (255, 110, 120), (9, 4, 12, 70), border_radius=8)           # блік
        pygame.draw.rect(s, (25, 30, 55), (10, 22, 24, 16), border_radius=5)            # лобове
        pygame.draw.rect(s, (25, 30, 55), (11, 52, 22, 10), border_radius=5)            # заднє
        pygame.draw.circle(s, (255, 245, 180), (12, 6), 3)                              # фари
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
        # рамка панелі
        pygame.draw.rect(screen, (14, 16, 28), PANEL.inflate(10, 10), border_radius=22)
        old_clip = screen.get_clip()
        screen.set_clip(PANEL)
        pygame.draw.rect(screen, (22, 128, 70), PANEL)                          # трава
        road = self.road_rect()
        pygame.draw.rect(screen, (48, 50, 60), road)                            # асфальт
        pygame.draw.rect(screen, (240, 240, 245), (road.x, PANEL.y, 5, PANEL.height))
        pygame.draw.rect(screen, (240, 240, 245), (road.right - 5, PANEL.y, 5, PANEL.height))
        for y in range(PANEL.y - 60, PANEL.bottom + 60, 60):                    # розмітка
            pygame.draw.rect(screen, (250, 210, 70), (road.centerx - 3, y + self.offset, 6, 32), border_radius=3)

        for o in self.obstacles:                                               # конуси
            r = self.obstacle_rect(o)
            pygame.draw.ellipse(screen, (0, 0, 0), r.inflate(6, -10).move(2, 10))
            pygame.draw.polygon(screen, (255, 120, 20), [(r.centerx, r.y), (r.right - 2, r.bottom - 4), (r.x + 2, r.bottom - 4)])
            pygame.draw.polygon(screen, (255, 255, 255), [(r.centerx - 6, r.y + 12), (r.centerx + 6, r.y + 12),
                                                          (r.centerx + 9, r.y + 18), (r.centerx - 9, r.y + 18)])
            pygame.draw.rect(screen, (30, 30, 35), (r.x, r.bottom - 5, r.width, 5), border_radius=2)

        cr = self.car_rect(steer)                                              # машинка
        tilted = pygame.transform.rotozoom(self.car, -steer * 14, 1.0)
        screen.blit(tilted, tilted.get_rect(center=cr.center))

        if self.flash > 0:                                                     # червоний спалах при ударі
            fl = pygame.Surface(PANEL.size, pygame.SRCALPHA)
            fl.fill((255, 40, 40, int(150 * self.flash / 0.35)))
            screen.blit(fl, PANEL.topleft)
        screen.set_clip(old_clip)

        # HUD
        draw_text(screen, fonts["small"], f"Очки: {self.score}", (PANEL.x + 14, PANEL.y + 10), (255, 255, 255))
        for i in range(3):
            col = (255, 70, 90) if i < self.lives else (70, 70, 85)
            pygame.draw.circle(screen, col, (PANEL.right - 24 - i * 24, PANEL.y + 22), 8)

        if self.over:
            ov = pygame.Surface(PANEL.size, pygame.SRCALPHA)
            ov.fill((0, 0, 0, 160))
            screen.blit(ov, PANEL.topleft)
            draw_text(screen, fonts["big"], "ГРА ЗАВЕРШЕНА", PANEL.center, (255, 255, 255), "center")
            draw_text(screen, fonts["small"], f"Очки: {self.score}", (PANEL.centerx, PANEL.centery + 40), (255, 220, 120), "center")
            draw_text(screen, fonts["small"], "R / Пробіл — знову", (PANEL.centerx, PANEL.centery + 72), (190, 195, 215), "center")


# ====================== ГОЛОВНИЙ ЦИКЛ ======================
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
    }

    background = make_background()
    wheel_img = make_wheel(WHEEL_R)
    glow = make_glow(300, (50, 110, 255))
    shadow = pygame.Surface((WHEEL_R * 2 + 80, WHEEL_R * 2 + 80), pygame.SRCALPHA)
    pygame.draw.ellipse(shadow, (0, 0, 0, 90), shadow.get_rect().inflate(-40, -40))

    game = Game()
    adc_raw = int(ADC_CENTER)
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

            # --- читання керма ---
            if demo:
                keys = pygame.key.get_pressed()
                target = (keys[pygame.K_RIGHT] or keys[pygame.K_d]) - (keys[pygame.K_LEFT] or keys[pygame.K_a])
                axis_val += max(-3 * dt, min(3 * dt, target - axis_val))
                adc_raw = int(ADC_CENTER + axis_val * ADC_CENTER)
            else:
                latest = None
                try:
                    while True:
                        rep = gamepad.read(64)
                        if not rep:
                            break
                        latest = rep
                except OSError:
                    demo, device_name = True, "Пристрій відключено"
                if latest and len(latest) >= 2:
                    adc_raw = max(0, min(ADC_MAX, latest[0] | (latest[1] << 8)))
                    axis_val = max(-1.0, min(1.0, (adc_raw - ADC_CENTER) / ADC_CENTER))

            if INVERT:
                axis_val_used = -axis_val
            else:
                axis_val_used = axis_val
            smoothed += (axis_val_used - smoothed) * min(1.0, dt * RESPONSE)
            angle = smoothed * MAX_ANGLE
            peak = max(peak * (1 - dt * 0.4), abs(smoothed))

            game.update(dt, smoothed)

            # --- малювання ---
            screen.blit(background, (0, 0))

            intensity = min(1.0, abs(smoothed))
            g = glow.copy()
            g.set_alpha(int(110 + 145 * intensity))
            screen.blit(g, g.get_rect(center=WHEEL_C))
            screen.blit(shadow, shadow.get_rect(center=(WHEEL_C[0] + 8, WHEEL_C[1] + 22)))

            rotated = pygame.transform.rotozoom(wheel_img, -angle, 1.0)
            screen.blit(rotated, rotated.get_rect(center=WHEEL_C))

            # шапка
            draw_text(screen, fonts["title"], "Steering Wheel", (30, 24))
            dot = (80, 230, 130) if not demo else (255, 190, 60)
            pygame.draw.circle(screen, dot, (36, 74), 6)
            draw_text(screen, fonts["tiny"], device_name, (50, 64), (170, 178, 205))

            # індикатори під кермом
            bar = pygame.Rect(WHEEL_C[0] - 200, 600, 400, 14)
            draw_round_bar(screen, bar, smoothed, (80, 220, 255), (255, 170, 80))
            draw_text(screen, fonts["small"], f"{angle:+.1f}°", (WHEEL_C[0], 575), (200, 255, 120), "center")
            draw_text(screen, fonts["tiny"], f"АЦП {adc_raw:04d}", (bar.x, 628), (150, 160, 190))
            draw_text(screen, fonts["tiny"], f"Вісь {smoothed:+.2f}", (bar.right, 628), (150, 160, 190), "topright")

            # маркер піку на смузі
            px = bar.centerx + int(math.copysign(peak, smoothed) * bar.width / 2)
            pygame.draw.circle(screen, (255, 255, 255), (px, bar.centery), 4)

            game.draw(screen, smoothed, fonts)
            draw_text(screen, fonts["tiny"], "ESC — вихід  |  R — рестарт", (PANEL.centerx, H - 22), (120, 128, 160), "center")

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