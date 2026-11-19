"""Terminal Snake for LixinOS MicroPython."""
import random
import select
import sys
import time

WIDTH = 20
HEIGHT = 14
KEYS = {"w": (0, -1), "a": (-1, 0), "s": (0, 1), "d": (1, 0)}
ARROWS = {"A": "w", "B": "s", "C": "d", "D": "a"}
EDGE = "+" + "-" * WIDTH + "+"


def food_for(snake):
    occupied = set(snake)
    free = [(x, y) for y in range(HEIGHT) for x in range(WIDTH)
            if (x, y) not in occupied]
    return free[random.randrange(len(free))] if free else None


def advance(snake, direction, food):
    x, y = snake[0]
    dx, dy = direction
    head = (x + dx, y + dy)
    grows = head == food
    body = snake if grows else snake[:-1]
    if not (0 <= head[0] < WIDTH and 0 <= head[1] < HEIGHT) or head in body:
        return None, False
    return [head] + body, grows


def key(poller, wait=-1):
    if not poller.poll(wait):
        return None
    char = sys.stdin.read(1)
    if char == "\x1b":
        if poller.poll(30) and sys.stdin.read(1) in ("[", "O") and poller.poll(30):
            return ARROWS.get(sys.stdin.read(1))
        return None
    return "q" if not char or char == "\x03" else char.lower()


def draw(snake, food, score, message):
    body = set(snake[1:])
    rows = [f"\x1b[H\x1b[36mSNAKE\x1b[0m   Score {score}\x1b[K", EDGE]
    for y in range(HEIGHT):
        line = ["|"]
        for x in range(WIDTH):
            point = (x, y)
            if point == snake[0]:
                line.append("\x1b[92m@\x1b[0m")
            elif point in body:
                line.append("\x1b[32mo\x1b[0m")
            elif point == food:
                line.append("\x1b[93m*\x1b[0m")
            else:
                line.append(" ")
        rows.append("".join(line) + "|")
    print("\n".join(rows + [EDGE, message + "\x1b[J"]), end="")


def play(poller, direction):
    x, y = WIDTH // 2, HEIGHT // 2
    dx, dy = direction
    snake = [(x - i * dx, y - i * dy) for i in range(3)]
    food = food_for(snake)
    score = 0
    print("\x1b[2J", end="")
    controls = "Arrows or WASD to move | Q to quit"
    while True:
        draw(snake, food, score, controls)
        next_direction = direction
        deadline = time.ticks_add(time.ticks_ms(), max(100, 240 - score * 20))
        while (wait := time.ticks_diff(deadline, time.ticks_ms())) > 0:
            command = key(poller, wait)
            if command == "q":
                return False
            if next_direction == direction and command in KEYS:
                dx, dy = KEYS[command]
                if direction[0] * dx + direction[1] * dy == 0:
                    next_direction = (dx, dy)
        direction = next_direction
        new_snake, grows = advance(snake, direction, food)
        if new_snake is None:
            draw(snake, food, score, "Game over | R to restart | Q to quit")
            return True
        snake = new_snake
        if grows:
            score += 1
            food = food_for(snake)
            if food is None:
                draw(snake, food, score, "Board cleared | R to restart | Q to quit")
                return True


def main():
    poller = select.poll()
    poller.register(sys.stdin, select.POLLIN)
    print("Snake ready. Arrows or WASD to start, Q to quit.")
    while (command := key(poller)) != "q":
        if command not in KEYS:
            continue
        print("\x1b[?25l", end="")
        try:
            while play(poller, KEYS[command]):
                while (answer := key(poller)) not in ("q", "r"):
                    pass
                if answer == "q":
                    break
            break
        finally:
            print("\x1b[0m\x1b[?25h")
    print("Bye.")


if __name__ == "__main__":
    main()
