#!/usr/bin/env python3
"""Generate a Markdown report of kernel module parameters."""

from __future__ import annotations

import argparse
import ast
import datetime as dt
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass
from typing import Dict, Iterable, List, Set, Tuple


DEFAULT_EXCLUDE_DIRS = {".git", "kernels"}
PARAM_MACROS = (
    "module_param",
    "module_param_named",
    "module_param_array",
    "module_param_string",
    "module_param_cb",
)
CATEGORY_ORDER = ["Common Public", "Common", "Client", "Server", "SIW", "Other"]

# Section order and names in NVMeshModuleParams template (for --template output).
TEMPLATE_SECTION_ORDER = [
    "nvmeiba",
    "nvmeibc",
    "nvmeib_common",
    "nvmeib_common_public",
    "nvmeibs",
    "siw",
]
# Map script category -> template section name (Other params go into nvmeibc).
CATEGORY_TO_TEMPLATE_SECTION: Dict[str, str] = {
    "nvmeiba": "nvmeiba",
    "Client": "nvmeibc",
    "Other": "nvmeibc",
    "Common": "nvmeib_common",
    "Common Public": "nvmeib_common_public",
    "Server": "nvmeibs",
    "SIW": "siw",
}


@dataclass
class ModuleParam:
    category: str
    source_file: str
    name: str
    var_name: str
    ptype: str
    perm_raw: str
    perm_state: str
    default_value: str
    description: str


@dataclass
class SymbolEntry:
    value: str
    condition: str


def normalize_space(s: str) -> str:
    return re.sub(r"\s+", " ", s.strip())


def strip_c_comments(s: str) -> str:
    return re.sub(r"//.*", "", re.sub(r"/\*.*?\*/", "", s, flags=re.S))


def parse_int_literal(expr: str) -> int | None:
    s = strip_wrapping_parens(normalize_space(expr))
    s = re.sub(r"([0-9A-Fa-fxX]+)[uUlL]+$", r"\1", s)
    if re.fullmatch(r"[+\-]?(?:0x[0-9A-Fa-f]+|\d+)", s):
        try:
            return int(s, 0)
        except ValueError:
            return None
    return None


def eval_int_expr(expr: str) -> int | None:
    """Safely evaluate simple integer expressions after symbol resolution."""
    cleaned = normalize_space(expr)
    # Remove integer suffixes like U/L/UL from numeric literals.
    cleaned = re.sub(r"\b(0x[0-9A-Fa-f]+|\d+)([uUlL]+)\b", r"\1", cleaned)
    # Normalize C bool literals for expression evaluation.
    cleaned = re.sub(r"\btrue\b", "1", cleaned)
    cleaned = re.sub(r"\bfalse\b", "0", cleaned)

    try:
        node = ast.parse(cleaned, mode="eval")
    except SyntaxError:
        return None

    def _eval(n: ast.AST) -> int:
        if isinstance(n, ast.Expression):
            return _eval(n.body)
        if isinstance(n, ast.Constant) and isinstance(n.value, int):
            return int(n.value)
        if isinstance(n, ast.UnaryOp):
            v = _eval(n.operand)
            if isinstance(n.op, ast.UAdd):
                return +v
            if isinstance(n.op, ast.USub):
                return -v
            if isinstance(n.op, ast.Invert):
                return ~v
            raise ValueError("unsupported unary")
        if isinstance(n, ast.BinOp):
            l = _eval(n.left)
            r = _eval(n.right)
            op = n.op
            if isinstance(op, ast.Add):
                return l + r
            if isinstance(op, ast.Sub):
                return l - r
            if isinstance(op, ast.Mult):
                return l * r
            if isinstance(op, ast.FloorDiv):
                return l // r
            if isinstance(op, ast.Div):
                return l // r
            if isinstance(op, ast.Mod):
                return l % r
            if isinstance(op, ast.LShift):
                return l << r
            if isinstance(op, ast.RShift):
                return l >> r
            if isinstance(op, ast.BitAnd):
                return l & r
            if isinstance(op, ast.BitOr):
                return l | r
            if isinstance(op, ast.BitXor):
                return l ^ r
            raise ValueError("unsupported binary")
        raise ValueError("unsupported node")

    try:
        return _eval(node)
    except Exception:
        return None


def collect_source_files(root: pathlib.Path, exclude_dirs: Iterable[str]) -> List[pathlib.Path]:
    excluded = set(exclude_dirs)
    files: List[pathlib.Path] = []
    for ext in ("*.c", "*.h"):
        files.extend(root.rglob(ext))
    out: List[pathlib.Path] = []
    for f in files:
        if set(f.relative_to(root).parts) & excluded:
            continue
        out.append(f)
    return sorted(out)


def split_top_level_args(s: str) -> List[str]:
    parts: List[str] = []
    cur: List[str] = []
    depth = 0
    in_str = False
    in_chr = False
    esc = False
    for ch in s:
        if in_str or in_chr:
            cur.append(ch)
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif (in_str and ch == '"') or (in_chr and ch == "'"):
                in_str = False
                in_chr = False
            continue
        if ch == '"':
            in_str = True
            cur.append(ch)
        elif ch == "'":
            in_chr = True
            cur.append(ch)
        elif ch == "(":
            depth += 1
            cur.append(ch)
        elif ch == ")":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur).strip())
    return parts


def parse_macro_calls(text: str, macro_name: str) -> List[str]:
    pattern = re.compile(rf"\b{re.escape(macro_name)}\s*\(")
    out: List[str] = []
    pos = 0
    while True:
        m = pattern.search(text, pos)
        if not m:
            break
        i = m.end()
        depth = 1
        in_str = False
        in_chr = False
        esc = False
        while i < len(text):
            ch = text[i]
            if in_str or in_chr:
                if esc:
                    esc = False
                elif ch == "\\":
                    esc = True
                elif (in_str and ch == '"') or (in_chr and ch == "'"):
                    in_str = False
                    in_chr = False
            else:
                if ch == '"':
                    in_str = True
                elif ch == "'":
                    in_chr = True
                elif ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if depth == 0:
                        j = i + 1
                        while j < len(text) and text[j].isspace():
                            j += 1
                        if j < len(text) and text[j] == ";":
                            out.append(text[m.end():i])
                            pos = j + 1
                        else:
                            pos = i + 1
                        break
            i += 1
        else:
            break
    return out


def decode_desc_text(raw: str) -> str:
    chunks = re.findall(r'"(?:\\.|[^"\\])*"', raw, flags=re.S)
    if not chunks:
        return normalize_space(raw)
    merged = ""
    for chunk in chunks:
        merged += bytes(chunk[1:-1], "utf-8").decode("unicode_escape")
    return normalize_space(merged)


def permission_state(perm_raw: str) -> str:
    p = perm_raw.strip()
    if re.fullmatch(r"0[0-7]+|[0-7]{3,4}", p):
        try:
            return "writable" if (int(p, 8) & 0o222) else "read-only"
        except ValueError:
            return "unknown"
    up = p.upper()
    if "W" in up or "WRITE" in up:
        return "writable"
    if "R" in up or "READ" in up or "S_IR" in up:
        return "read-only"
    return "unknown"


def infer_category(rel_path: str) -> str:
    if rel_path.startswith("common_public/"):
        return "Common Public"
    if rel_path.startswith("common/"):
        return "Common"
    if rel_path.startswith("clnt/atom/"):
        return "nvmeiba"
    if rel_path.startswith("clnt/"):
        return "Client"
    if rel_path.startswith("srv/"):
        return "Server"
    if rel_path.startswith("softiwarp/"):
        return "SIW"
    return "Other"


def parse_preprocessor_defines(raw: str) -> Dict[str, List[SymbolEntry]]:
    out: Dict[str, List[SymbolEntry]] = {}
    lines = raw.splitlines()
    i = 0
    cond_stack: List[str] = []
    while i < len(lines):
        merged = lines[i]
        while merged.rstrip().endswith("\\") and i + 1 < len(lines):
            merged = merged.rstrip()[:-1] + lines[i + 1]
            i += 1
        i += 1
        if not merged.lstrip().startswith("#"):
            continue
        if m := re.match(r"^\s*#\s*ifdef\s+([A-Za-z_]\w*)", merged):
            cond_stack.append(f"defined({m.group(1)})")
            continue
        if m := re.match(r"^\s*#\s*ifndef\s+([A-Za-z_]\w*)", merged):
            cond_stack.append(f"!defined({m.group(1)})")
            continue
        if m := re.match(r"^\s*#\s*if\s+(.+)$", merged):
            cond_stack.append(normalize_space(m.group(1)))
            continue
        if m := re.match(r"^\s*#\s*elif\s+(.+)$", merged):
            if cond_stack:
                cond_stack[-1] = normalize_space(m.group(1))
            continue
        if re.match(r"^\s*#\s*else\b", merged):
            if cond_stack:
                cond_stack[-1] = f"else({cond_stack[-1]})"
            continue
        if re.match(r"^\s*#\s*endif\b", merged):
            if cond_stack:
                cond_stack.pop()
            continue
        m = re.match(r"^\s*#\s*define\s+([A-Za-z_]\w*)(.*)$", merged)
        if not m:
            continue
        # Function-like macro only when '(' is immediate after macro name.
        if m.group(2).startswith("("):
            continue
        rest = m.group(2).lstrip()
        cond = " && ".join(cond_stack) if cond_stack else ""
        value = normalize_space(strip_c_comments(rest)) or "1"
        out.setdefault(m.group(1), []).append(SymbolEntry(value=value, condition=cond))
    return out


def parse_enums(raw: str) -> Dict[str, List[SymbolEntry]]:
    out: Dict[str, List[SymbolEntry]] = {}
    text = strip_c_comments(raw)
    pos = 0
    while True:
        m = re.search(r"\benum\b[^;{]*\{", text[pos:])
        if not m:
            break
        start = pos + m.end()
        depth = 1
        i = start
        while i < len(text) and depth > 0:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        if depth != 0:
            break
        next_value = 0
        can_auto_increment = True
        for item in split_top_level_args(text[start:i - 1]):
            item = item.strip()
            if not item:
                continue
            mm = re.match(r"^\s*([A-Za-z_]\w*)\s*=\s*(.+)$", item, flags=re.S)
            if mm:
                name = mm.group(1)
                expr = normalize_space(mm.group(2))
                out.setdefault(name, []).append(SymbolEntry(value=expr, condition=""))
                parsed = parse_int_literal(expr)
                if parsed is None:
                    can_auto_increment = False
                else:
                    can_auto_increment = True
                    next_value = parsed + 1
                continue

            mm = re.match(r"^\s*([A-Za-z_]\w*)\s*$", item)
            if not mm:
                continue
            name = mm.group(1)
            if can_auto_increment:
                out.setdefault(name, []).append(
                    SymbolEntry(value=str(next_value), condition="")
                )
                next_value += 1
            else:
                # Keep symbol discoverable even if value cannot be inferred.
                out.setdefault(name, []).append(
                    SymbolEntry(value=name, condition="")
                )
        pos = i
    return out


def merge_symbol_maps(*maps: Dict[str, List[SymbolEntry]]) -> Dict[str, List[SymbolEntry]]:
    out: Dict[str, List[SymbolEntry]] = {}
    for m in maps:
        for k, vals in m.items():
            out.setdefault(k, []).extend(vals)
    return out


def strip_wrapping_parens(expr: str) -> str:
    s = expr.strip()
    while s.startswith("(") and s.endswith(")"):
        depth = 0
        valid = True
        for idx, ch in enumerate(s):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0 and idx != len(s) - 1:
                    valid = False
                    break
        if not valid or depth != 0:
            break
        s = s[1:-1].strip()
    return s


def extract_symbol_identifier(expr: str) -> str:
    s = strip_wrapping_parens(expr)
    if s in {"true", "false", "NULL"}:
        return ""
    if re.fullmatch(r"[+\-]?(?:0x[0-9A-Fa-f]+|\d+)(?:[UuLl]*)?", s):
        return ""
    m = re.match(r"^(?:\(\s*[A-Za-z_][A-Za-z0-9_ \*\t]+\s*\)\s*)*([A-Za-z_]\w*)$", s)
    if m:
        ident = m.group(1)
        return "" if ident in {"true", "false", "NULL"} else ident
    m = re.match(r"^[+\-~]?\s*([A-Za-z_]\w*)$", s)
    if not m:
        return ""
    ident = m.group(1)
    return "" if ident in {"true", "false", "NULL"} else ident


def resolve_symbol(
    name: str,
    local_map: Dict[str, List[SymbolEntry]],
    global_map: Dict[str, List[SymbolEntry]],
    seen: Set[str],
) -> str:
    if name in seen:
        return name
    entries = local_map.get(name) or global_map.get(name) or []
    if not entries:
        return name

    resolved: List[Tuple[str, str]] = []
    for ent in entries:
        value = ent.value
        next_name = extract_symbol_identifier(value)
        if next_name and next_name != name:
            value = resolve_symbol(next_name, local_map, global_map, seen | {name})
        resolved.append((ent.condition or "default", value))

    uniq = {normalize_space(strip_wrapping_parens(v)) for _, v in resolved}
    if len(uniq) == 1:
        return resolved[0][1]

    dedup: List[Tuple[str, str]] = []
    seen_pairs: Set[Tuple[str, str]] = set()
    for cond, value in resolved:
        pair = (normalize_space(cond), normalize_space(strip_wrapping_parens(value)))
        if pair in seen_pairs:
            continue
        seen_pairs.add(pair)
        dedup.append((cond, value))

    parts: List[str] = []
    for cond, value in dedup:
        if cond.startswith("else(") and cond.endswith(")"):
            parts.append(f"otherwise {value}")
        elif not cond or cond == "default":
            parts.append(f"default {value}")
        else:
            parts.append(f"if {cond} then {value}")
    return "; ".join(parts)


def _resolve_initializer(
    expr: str,
    local_map: Dict[str, List[SymbolEntry]],
    global_map: Dict[str, List[SymbolEntry]],
    param_type: str,
) -> str:
    is_bool_type = param_type.strip() == "bool"
    def split_top_level_ternary(s: str) -> tuple[str, str, str] | None:
        depth = 0
        in_str = False
        in_chr = False
        esc = False
        q_idx = -1
        c_idx = -1
        for i, ch in enumerate(s):
            if in_str or in_chr:
                if esc:
                    esc = False
                elif ch == "\\":
                    esc = True
                elif (in_str and ch == '"') or (in_chr and ch == "'"):
                    in_str = in_chr = False
                continue
            if ch == '"':
                in_str = True
                continue
            if ch == "'":
                in_chr = True
                continue
            if ch == "(":
                depth += 1
                continue
            if ch == ")":
                depth -= 1
                continue
            if depth == 0 and ch == "?" and q_idx == -1:
                q_idx = i
                continue
            if depth == 0 and ch == ":" and q_idx != -1:
                c_idx = i
                break
        if q_idx == -1 or c_idx == -1:
            return None
        return s[:q_idx], s[q_idx + 1:c_idx], s[c_idx + 1:]

    def resolve_ident_tokens(s: str) -> str:
        def repl(m: re.Match[str]) -> str:
            tok = m.group(0)
            if tok in {"true", "false", "NULL"}:
                return tok
            resolved = resolve_symbol(tok, local_map, global_map, set())
            # Replace only with simple resolved values.
            if ";" in resolved:
                return tok
            return resolved
        return re.sub(r"\b[A-Za-z_]\w*\b", repl, s)

    expr = normalize_space(strip_c_comments(expr))
    ternary = split_top_level_ternary(expr)
    if ternary:
        cond_raw, t_raw, f_raw = ternary
        cond_resolved = normalize_space(resolve_ident_tokens(cond_raw))
        t_resolved = normalize_space(resolve_ident_tokens(t_raw))
        f_resolved = normalize_space(resolve_ident_tokens(f_raw))
        cond_int = parse_int_literal(cond_resolved)
        if cond_int is not None:
            chosen = t_resolved if cond_int != 0 else f_resolved
            evaluated = eval_int_expr(chosen)
            if evaluated is not None:
                if is_bool_type:
                    return "true" if evaluated != 0 else "false"
                return str(evaluated)
            return chosen
        return f"{cond_resolved} ? {t_resolved} : {f_resolved}"

    ident = extract_symbol_identifier(expr)
    if ident:
        resolved = resolve_symbol(ident, local_map, global_map, set())
        if resolved != ident:
            return resolved
    resolved_expr = normalize_space(resolve_ident_tokens(expr))
    evaluated = eval_int_expr(resolved_expr)
    if evaluated is not None:
        if is_bool_type:
            return "true" if evaluated != 0 else "false"
        return str(evaluated)
    return resolved_expr


def find_default_value(
    file_text: str,
    var_name: str,
    param_type: str,
    local_map: Dict[str, List[SymbolEntry]],
    global_map: Dict[str, List[SymbolEntry]],
) -> str:
    searchable = strip_c_comments(file_text)
    base = (
        rf"^[ \t]*[A-Za-z_][A-Za-z0-9_ \t\*\(\)]*\b{re.escape(var_name)}\b"
        rf"(?:\s*\[[^\]]*\])?\s*"
    )
    if m := re.search(base + r"=\s*(.+?);[ \t]*$", searchable, flags=re.M):
        return _resolve_initializer(m.group(1), local_map, global_map, param_type)
    if m := re.search(base + r"=\s*(.*?);", searchable, flags=re.M | re.S):
        return _resolve_initializer(m.group(1), local_map, global_map, param_type)
    if re.search(base + r";[ \t]*$", searchable, flags=re.M):
        return "(no explicit initializer)"
    return "(not found)"


def build_desc_map(file_text: str) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for args in parse_macro_calls(file_text, "MODULE_PARM_DESC"):
        parts = split_top_level_args(args)
        if len(parts) < 2:
            continue
        out[parts[0].strip()] = decode_desc_text(",".join(parts[1:]))
    return out


def parse_param_macro(macro: str, args: str) -> Tuple[str, str, str, str] | None:
    parts = split_top_level_args(args)
    if macro == "module_param" and len(parts) >= 3:
        return parts[0], parts[0], parts[1], parts[2]
    if macro == "module_param_named" and len(parts) >= 4:
        return parts[0], parts[1], parts[2], parts[3]
    if macro == "module_param_array" and len(parts) >= 4:
        return parts[0], parts[0], f"{parts[1]}[]", parts[3]
    if macro == "module_param_string" and len(parts) >= 4:
        return parts[0], parts[1], "string", parts[3]
    if macro == "module_param_cb" and len(parts) >= 4:
        return parts[0], parts[2], "callback", parts[3]
    return None


def extract_params_for_file(
    path: pathlib.Path,
    root: pathlib.Path,
    global_symbols: Dict[str, List[SymbolEntry]],
) -> List[ModuleParam]:
    text = path.read_text(encoding="utf-8", errors="replace")
    clean_text = strip_c_comments(text)
    rel = str(path.relative_to(root))
    local_symbols = merge_symbol_maps(parse_preprocessor_defines(clean_text), parse_enums(clean_text))
    desc_map = build_desc_map(clean_text)
    params: List[ModuleParam] = []
    for macro in PARAM_MACROS:
        for args in parse_macro_calls(clean_text, macro):
            parsed = parse_param_macro(macro, args)
            if not parsed:
                continue
            pname, var_name, ptype, perm = [p.strip() for p in parsed]
            params.append(
                ModuleParam(
                    category=infer_category(rel),
                    source_file=rel,
                    name=pname,
                    var_name=var_name,
                    ptype=ptype,
                    perm_raw=perm,
                    perm_state=permission_state(perm),
                    default_value=find_default_value(
                        text, var_name, ptype, local_symbols, global_symbols
                    ),
                    description=desc_map.get(pname, "(no MODULE_PARM_DESC found)"),
                )
            )
    return params


def md_escape(s: str) -> str:
    return s.replace("|", r"\|").replace("\n", " ")


def emit_table(lines: List[str], rows: List[ModuleParam]) -> None:
    lines.append("| Name | Description | Type | Permissions | Defaults |")
    lines.append("|---|---|---|---|---|")
    for r in rows:
        lines.append(
            f"| `{md_escape(r.name)}` | {md_escape(r.description)} | `{md_escape(r.ptype)}` | "
            f"{md_escape(f'{r.perm_state} (`{r.perm_raw}`)')} | `{md_escape(r.default_value)}` |"
        )
    lines.append("")


def emit_template_table(rows: List[ModuleParam]) -> List[str]:
    """Emit 5-column table (name, description, type, permission, default) for template doc."""
    lines = [
        "| Name | Description | Type | Permissions | Defaults |",
        "|---|---|---|---|---|",
    ]
    for r in rows:
        lines.append(
            f"| `{md_escape(r.name)}` | {md_escape(r.description)} | `{md_escape(r.ptype)}` | "
            f"{md_escape(f'{r.perm_state} (`{r.perm_raw}`)')} | `{md_escape(r.default_value)}` |"
        )
    lines.append("")
    return lines


def git_describe(root: pathlib.Path) -> str:
    """Return git describe output for the repo, or a fallback version string."""
    try:
        out = subprocess.run(
            ["git", "describe", "--always", "--tags", "--dirty"],
            cwd=root,
            capture_output=True,
            text=True,
            timeout=5,
        )
        if out.returncode == 0 and out.stdout:
            return out.stdout.strip()
    except (subprocess.SubprocessError, FileNotFoundError):
        pass
    return "unknown"


def write_report_from_template(
    params: Iterable[ModuleParam],
    template_path: pathlib.Path,
    out_file: pathlib.Path,
    version: str,
) -> None:
    """Build report using template for wrapper (TOC, preface, sections) and code for tables."""
    by_cat: Dict[str, List[ModuleParam]] = {}
    for p in params:
        by_cat.setdefault(p.category, []).append(p)

    # Map category -> section -> sorted list of params (merge Client + Other into nvmeibc).
    by_section: Dict[str, List[ModuleParam]] = {}
    for section in TEMPLATE_SECTION_ORDER:
        by_section[section] = []
    for cat, rows in by_cat.items():
        section = CATEGORY_TO_TEMPLATE_SECTION.get(cat, "nvmeibc")
        by_section[section].extend(rows)
    for section in TEMPLATE_SECTION_ORDER:
        by_section[section].sort(key=lambda x: x.name)

    template_text = template_path.read_text(encoding="utf-8", errors="replace")

    # Replace version in title and TOC (e.g. "3.4.0" -> version from git describe).
    title_version = version.split("-")[0] if version != "unknown" else "111.111.111"
    anchor_version = title_version.replace(".", "")
    template_text = template_text.replace(
        "NVMesh 3.4.0 Module Params Guide",
        f"NVMesh {title_version} Module Params Guide",
    )
    template_text = template_text.replace(
        "#nvmesh-340-module-params-guide",
        f"#nvmesh-{anchor_version}-module-params-guide",
    )

    # Replace each ## section's table with generated table.
    for section in TEMPLATE_SECTION_ORDER:
        start_marker = f"## {section}"
        start_idx = template_text.find(start_marker)
        if start_idx == -1:
            continue
        table_start = template_text.find("| Parameter | Description |", start_idx)
        if table_start == -1:
            continue
        # Table ends at the next "\n## " or end of file.
        table_end = template_text.find("\n## ", table_start)
        if table_end == -1:
            table_end = len(template_text)
        else:
            table_end += 1  # keep the newline before ##

        new_table_lines = emit_template_table(by_section[section])
        new_table = "\n".join(new_table_lines)
        template_text = (
            template_text[:table_start] + new_table.rstrip() + "\n\n" + template_text[table_end:]
        )

    out_file.write_text(template_text, encoding="utf-8")


def write_report(params: Iterable[ModuleParam], out_file: pathlib.Path, group_by: str) -> None:
    by_cat: Dict[str, List[ModuleParam]] = {}
    for p in params:
        by_cat.setdefault(p.category, []).append(p)

    lines = [
        "# Kernel Module Parameters",
        "",
        f"_Generated: {dt.datetime.now().isoformat(timespec='seconds')}_",
        "",
        "Columns: `Name`, `Description` (from `MODULE_PARM_DESC`), `Type`, "
        "`Permissions` (read-only or writable), `Defaults` (best-effort from variable declaration).",
        "",
        f"Grouping mode: `{group_by}`",
        "",
    ]

    for cat in CATEGORY_ORDER:
        if cat not in by_cat:
            continue
        rows = by_cat[cat]
        lines.extend([f"## {cat}", ""])
        if group_by == "module":
            emit_table(lines, sorted(rows, key=lambda x: x.name))
            continue
        by_file: Dict[str, List[ModuleParam]] = {}
        for p in rows:
            by_file.setdefault(p.source_file, []).append(p)
        for file_name in sorted(by_file):
            lines.extend([f"### `{file_name}`", ""])
            emit_table(lines, sorted(by_file[file_name], key=lambda x: x.name))

    out_file.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Extract module parameters and generate a Markdown report.")
    p.add_argument("--root", default=".", help="Repository root to scan (default: current directory).")
    p.add_argument("--output", default=None, help="Output Markdown file path. Default: module_params.md, or NVMeshModuleParams-<git describe>.md when using --template.")
    p.add_argument(
        "--exclude-dir",
        action="append",
        default=[],
        help="Directory name to exclude from scan (can be repeated). Default excludes: .git, kernels",
    )
    p.add_argument(
        "--group-by",
        choices=("module", "file"),
        default="module",
        help="Output grouping style: module (Common/Client/Server/SIW) or file. Ignored when --template is used.",
    )
    p.add_argument(
        "--template",
        metavar="FILE",
        default="doc-templates/NVMeshModuleParams-template.md",
        help="Use FILE as template (e.g. NVMeshModuleParams-3.4.0.md). Tables are generated from code; TOC, preface, section order come from template. Output defaults to NVMeshModuleParams-<git describe>.md.",
    )
    return p.parse_args()


def main() -> int:
    args = parse_args()
    root = pathlib.Path(args.root).resolve()
    use_template = args.template is not None
    if use_template:
        template_path = pathlib.Path(args.template).resolve()
        if not template_path.is_file():
            print(f"Template file not found: {template_path}", file=sys.stderr)
            return 1
        if args.output is None:
            version = git_describe(root)
            # Sanitize for filename: replace / with -
            version_safe = version.replace("/", "-")
            out_file = root / f"NVMeshModuleParams-{version_safe}.md"
        else:
            out_file = pathlib.Path(args.output).resolve()
    else:
        out_file = pathlib.Path(args.output or "module_params.md").resolve()
        template_path = None

    files = collect_source_files(root, set(DEFAULT_EXCLUDE_DIRS) | set(args.exclude_dir))

    global_symbols: Dict[str, List[SymbolEntry]] = {}
    for fp in files:
        try:
            raw = fp.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        global_symbols = merge_symbol_maps(global_symbols, parse_preprocessor_defines(raw), parse_enums(raw))

    params: List[ModuleParam] = []
    for fp in files:
        try:
            params.extend(extract_params_for_file(fp, root, global_symbols))
        except OSError:
            continue

    if use_template:
        version = git_describe(root)
        write_report_from_template(params, template_path, out_file, version)
    else:
        write_report(params, out_file, args.group_by)
    print(f"Wrote {len(params)} params to {out_file}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
