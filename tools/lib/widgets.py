"""
Tritium-OS Widget Helpers
==========================
Utilities for filtering and inspecting LVGL widget trees returned by the API.
"""


def type_match(wtype: str, *names: str) -> bool:
    """Check if widget type matches any name (with or without lv_ prefix)."""
    bare = wtype.removeprefix("lv_")
    return bare in names or wtype in names


def extract_by_type(widgets: list, wtype: str) -> list:
    """Filter widgets by type name (matches with or without lv_ prefix)."""
    bare = wtype.removeprefix("lv_")
    return [w for w in widgets if w.get("type") in (wtype, bare, f"lv_{bare}")]


def extract_interactive(widgets: list) -> list:
    return [w for w in widgets if w.get("clickable")]


def extract_buttons(widgets: list) -> list:
    return extract_by_type(widgets, "btn")


def extract_labels(widgets: list) -> list:
    return extract_by_type(widgets, "label")


def extract_sliders(widgets: list) -> list:
    return extract_by_type(widgets, "slider")


def extract_switches(widgets: list) -> list:
    return extract_by_type(widgets, "switch")


def extract_bars(widgets: list) -> list:
    return extract_by_type(widgets, "bar")


def extract_dropdowns(widgets: list) -> list:
    return extract_by_type(widgets, "dropdown")


def extract_values(widgets: list) -> dict:
    """Extract all readable values from widget tree."""
    values = {}
    for w in widgets:
        wid = w.get("id", "")
        wtype = w.get("type", "")
        text = w.get("text", "")

        if type_match(wtype, "label") and text:
            values[wid] = {"type": "label", "text": text}
        elif type_match(wtype, "slider"):
            values[wid] = {"type": "slider", "value": w.get("value"),
                           "min": w.get("min"), "max": w.get("max")}
        elif type_match(wtype, "bar"):
            values[wid] = {"type": "bar", "value": w.get("value"),
                           "min": w.get("min"), "max": w.get("max")}
        elif type_match(wtype, "switch", "checkbox"):
            values[wid] = {"type": wtype.removeprefix("lv_"),
                           "checked": w.get("checked")}
        elif type_match(wtype, "dropdown"):
            values[wid] = {"type": "dropdown",
                           "selected": w.get("selected"),
                           "selected_text": w.get("selected_text")}
        elif type_match(wtype, "textarea"):
            values[wid] = {"type": "textarea", "value": w.get("value", "")}
        elif type_match(wtype, "btn") and text:
            values[wid] = {"type": "button", "text": text}
        elif text:
            values[wid] = {"type": wtype, "text": text}
    return values
