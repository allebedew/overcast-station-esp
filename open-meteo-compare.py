#!/usr/bin/env python3
"""
Сравнение текущей погоды в Кракове по разным моделям Open-Meteo.
Запуск: python open-meteo-compare.py
Зависимости: requests (pip install requests)
"""

import requests

LAT = 50.06  # Краков
LON = 19.94

CURRENT_VARS = [
    "temperature_2m",
    "precipitation",
    "rain",
    "showers",
    "snowfall",
    "weather_code",
    "wind_speed_10m",
    "wind_direction_10m",
    "uv_index",
]

# (label, model) — model None means the request omits `models`, letting the API pick.
MODELS = [
    ("auto (без models)", None),
    ("best_match", "best_match"),
    ("ecmwf_ifs", "ecmwf_ifs"),
    ("icon_seamless", "icon_seamless"),
    ("icon_d2", "icon_d2"),
    ("gfs_seamless", "gfs_seamless"),
    ("gem_seamless", "gem_seamless"),
    ("ukmo_seamless", "ukmo_seamless"),
    ("meteofrance_seamless", "meteofrance_seamless"),
]

# (title, width, variable, decimals, suffix) — header and rows share it so columns stay aligned.
COLUMNS = [
    ("Темп", 7, "temperature_2m", 1, "°"),
    ("Осадки", 9, "precipitation", 1, " мм"),
    ("Дождь", 8, "rain", 1, " мм"),
    ("Ливень", 8, "showers", 1, " мм"),
    ("Снег", 7, "snowfall", 1, " см"),
    ("Ветер", 8, "wind_speed_10m", 1, ""),
    ("Напр", 6, "wind_direction_10m", 0, "°"),
    ("УФ", 6, "uv_index", 1, ""),
]

NAME_W = 22
TIME_W = 7   # "HH:MM"
INT_W = 8    # "15 мин" — resampling step the API reports for `current`

WEATHER_CODES = {
    0: "Ясно", 1: "Преим. ясно", 2: "Переменная облачность", 3: "Пасмурно",
    45: "Туман", 48: "Изморозь",
    51: "Морось слабая", 53: "Морось", 55: "Морось сильная",
    61: "Дождь слабый", 63: "Дождь", 65: "Дождь сильный",
    71: "Снег слабый", 73: "Снег", 75: "Снег сильный",
    80: "Ливень слабый", 81: "Ливень", 82: "Ливень сильный",
    95: "Гроза", 96: "Гроза с градом", 99: "Гроза с сильным градом",
}


def fetch(model: str | None) -> dict | None:
    url = "https://api.open-meteo.com/v1/forecast"
    params = {
        "latitude": LAT,
        "longitude": LON,
        "current": ",".join(CURRENT_VARS),
        "timezone": "auto",
    }
    if model is not None:
        params["models"] = model
    try:
        r = requests.get(url, params=params, timeout=10)
        # Open-Meteo puts the real cause in the JSON body of a 4xx response.
        if not r.ok:
            try:
                return {"error": r.json().get("reason", r.reason)}
            except ValueError:
                return {"error": f"HTTP {r.status_code}"}
        return r.json()
    except Exception as e:
        return {"error": str(e)}


def fmt(value, width, decimals, suffix):
    text = "-" if value is None else f"{value:.{decimals}f}{suffix}"
    return f"{text:>{width}}"


def main():
    print(f"Погода сейчас в Кракове ({LAT}, {LON})\n")
    header = (
        f"{'Модель':<{NAME_W}}{'Время':>{TIME_W}}{'Инт':>{INT_W}}"
        + "".join(f"{title:>{width}}" for title, width, *_ in COLUMNS)
        + "  Состояние"
    )
    print(header)
    print("-" * len(header))

    for label, model in MODELS:
        data = fetch(model)
        if "error" in data:
            print(f"{label:<{NAME_W}} [!] {data['error']}")
            continue
        if "current" not in data:
            print(f"{label:<{NAME_W}} нет данных")
            continue

        c = data["current"]
        code = c.get("weather_code")
        desc = WEATHER_CODES.get(code, f"код {code}" if code is not None else "н/д")
        rain_now = (c.get("precipitation") or 0) > 0
        clock = (c.get("time") or "").split("T")[-1] or "-"
        interval = c.get("interval")
        interval = f"{interval // 60} мин" if interval else "-"

        line = f"{label:<{NAME_W}}{clock:>{TIME_W}}{interval:>{INT_W}}" + "".join(
            fmt(c.get(var), width, decimals, suffix)
            for _, width, var, decimals, suffix in COLUMNS
        ) + f"  {desc}" + ("  ☔" if rain_now else "")
        print(line)

    print("\nГотово. '☔' — модель показывает осадки прямо сейчас.")


if __name__ == "__main__":
    main()
