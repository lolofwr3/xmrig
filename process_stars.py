"""
╔══════════════════════════════════════════════════════════════════╗
║        MILKY WAY STAR PROCESSOR — Gaia DR3 + HYG Pipeline       ║
║  Fase 1: Descarga, limpieza y exportación a JSON optimizado      ║
╚══════════════════════════════════════════════════════════════════╝

DEPENDENCIAS (instalar una vez):
    pip install astropy pandas requests numpy tqdm astroquery

ESTRATEGIA DE DATOS:
  - Fuente primaria : Gaia DR3 via TAP/ADQL (astroquery)
                      https://gea.esac.esa.int/tap-server/tap
  - Fuente de apoyo : Catálogo HYG v3 (CSV ligero, ~120K estrellas)
                      https://github.com/astronexus/HYG-Database
  - Proyección      : Coordenadas esféricas (ra, dec, parallax)
                      → coordenadas cartesianas XYZ en parsecs
  - Color estelar   : temperatura efectiva (teff) → color RGB
                      usando la escala de Planck / Ballesteros (2012)

SALIDA:
  stars.json  →  [{x, y, z, r, g, b, size, name?}, ...]
  Tamaño estimado: ~8–15 MB para 200K estrellas (sin comprimir)
"""

import json
import math
import os
import struct
import sys

import numpy as np
import pandas as pd
import requests
from tqdm import tqdm

# ──────────────────────────────────────────────────────────────────
# CONFIGURACIÓN GLOBAL
# ──────────────────────────────────────────────────────────────────
CONFIG = {
    # Número máximo de estrellas a procesar (ajusta según RAM/GPU)
    # 200K es un buen balance calidad/rendimiento para Three.js
    "MAX_STARS": 200_000,

    # Distancia máxima en parsecs (1 kpc ≈ 3260 años-luz)
    # 8000 pc cubre bien la parte visible de la Vía Láctea
    "MAX_DISTANCE_PC": 8_000,

    # Archivos de salida
    "OUTPUT_JSON": "stars.json",
    "OUTPUT_BINARY": "stars.bin",   # Opcional: binario más ligero

    # URL del catálogo HYG (fallback rápido sin necesitar cuenta)
    "HYG_URL": "https://raw.githubusercontent.com/astronexus/HYG-Database/master/hyg/v3/hyg.csv",

    # Usar Gaia DR3 vía astroquery (requiere conexión a ESA)
    "USE_GAIA": True,

    # Exportar también binario (Float32Array) para carga ultra-rápida
    "EXPORT_BINARY": True,
}


# ──────────────────────────────────────────────────────────────────
# MÓDULO 1: DESCARGA DE DATOS
# ──────────────────────────────────────────────────────────────────

def download_hyg_catalog() -> pd.DataFrame:
    """
    Descarga el catálogo HYG v3 (~120K estrellas con coordenadas XYZ
    ya calculadas, magnitud y tipo espectral).
    Es la opción más rápida para empezar: ~10 MB de CSV.
    """
    cache_file = "hyg_v3_cache.csv"

    if os.path.exists(cache_file):
        print(f"[HYG] Cargando desde caché local: {cache_file}")
        return pd.read_csv(cache_file, low_memory=False)

    print("[HYG] Descargando catálogo HYG v3 desde GitHub...")
    response = requests.get(CONFIG["HYG_URL"], stream=True, timeout=60)
    response.raise_for_status()

    total = int(response.headers.get("content-length", 0))
    with open(cache_file, "wb") as f, tqdm(
        total=total, unit="B", unit_scale=True, desc="HYG download"
    ) as bar:
        for chunk in response.iter_content(chunk_size=8192):
            f.write(chunk)
            bar.update(len(chunk))

    print("[HYG] Descarga completa.")
    return pd.read_csv(cache_file, low_memory=False)


def query_gaia_dr3(limit: int = 500_000) -> pd.DataFrame:
    """
    Consulta Gaia DR3 via ADQL/TAP usando astroquery.
    Selecciona estrellas con paralaje positivo y confiable,
    temperatura efectiva y magnitud.

    ADQL equivalente:
        SELECT source_id, ra, dec, parallax, parallax_over_error,
               phot_g_mean_mag, teff_gspphot, bp_rp
        FROM gaiadr3.gaia_source
        WHERE parallax > 0.125          -- distancia < 8 kpc
          AND parallax_over_error > 5   -- paralaje confiable
          AND phot_g_mean_mag < 15      -- estrellas visibles
        ORDER BY phot_g_mean_mag ASC
        TOP {limit}
    """
    try:
        from astroquery.gaia import Gaia
    except ImportError:
        print("[GAIA] astroquery no instalado. Usando HYG como fallback.")
        return None

    print(f"[GAIA] Consultando Gaia DR3 (esto puede tardar 2–5 minutos)...")

    # Filtramos por paralaje > 0.125 mas → distancia < 8000 pc
    query = f"""
    SELECT TOP {limit}
        source_id,
        ra, dec,
        parallax,
        parallax_over_error,
        phot_g_mean_mag,
        teff_gspphot,
        bp_rp,
        nu_eff_used_in_astrometry
    FROM gaiadr3.gaia_source
    WHERE parallax > 0.125
      AND parallax_over_error > 5
      AND phot_g_mean_mag IS NOT NULL
      AND phot_g_mean_mag < 15
    ORDER BY phot_g_mean_mag ASC
    """

    try:
        Gaia.ROW_LIMIT = limit
        job = Gaia.launch_job(query)
        result = job.get_results().to_pandas()
        print(f"[GAIA] Descargadas {len(result):,} estrellas.")
        return result
    except Exception as e:
        print(f"[GAIA] Error al consultar: {e}\n[GAIA] Usando HYG como fallback.")
        return None


# ──────────────────────────────────────────────────────────────────
# MÓDULO 2: CONVERSIÓN DE COORDENADAS
# ──────────────────────────────────────────────────────────────────

def equatorial_to_cartesian(ra_deg: np.ndarray,
                             dec_deg: np.ndarray,
                             distance_pc: np.ndarray) -> tuple:
    """
    Convierte (RA, Dec, distancia) → coordenadas cartesianas (X, Y, Z).

    Sistema de referencia: galactocéntrico aproximado
      X: apunta hacia el centro galáctico (l=0°, b=0°)
      Y: en el plano galáctico, perpendicular a X
      Z: perpendicular al plano galáctico (Norte galáctico)

    Fórmulas:
      ra_rad  = ra  * π/180
      dec_rad = dec * π/180
      x = d * cos(dec_rad) * cos(ra_rad)
      y = d * cos(dec_rad) * sin(ra_rad)
      z = d * sin(dec_rad)
    """
    ra_rad  = np.radians(ra_deg)
    dec_rad = np.radians(dec_deg)

    cos_dec = np.cos(dec_rad)
    x = distance_pc * cos_dec * np.cos(ra_rad)
    y = distance_pc * cos_dec * np.sin(ra_rad)
    z = distance_pc * np.sin(dec_rad)

    return x, y, z


def parallax_to_distance(parallax_mas: np.ndarray) -> np.ndarray:
    """
    Distancia en parsecs = 1000 / paralaje_en_milisegundos_de_arco.
    Solo válido para paralajes positivos y con buena S/N.
    """
    with np.errstate(divide="ignore", invalid="ignore"):
        distance = np.where(parallax_mas > 0, 1000.0 / parallax_mas, np.nan)
    return distance


# ──────────────────────────────────────────────────────────────────
# MÓDULO 3: TEMPERATURA → COLOR RGB
# ──────────────────────────────────────────────────────────────────

def teff_to_rgb(teff: float) -> tuple:
    """
    Convierte temperatura efectiva (Kelvin) a color RGB [0–255].
    Implementa la aproximación de Ballesteros (2012):
    https://arxiv.org/abs/1201.1809

    Tabla de referencia de tipos espectrales:
      O:  > 30,000K  → azul brillante   (#9BB2FF)
      B: 10,000–30K  → azul-blanco      (#AAC0FF)
      A:  7,500–10K  → blanco           (#CAD7FF)
      F:  6,000–7.5K → amarillo-blanco  (#F8F7FF)
      G:  5,200–6K   → amarillo (Sol)   (#FFF4EA)
      K:  3,700–5.2K → naranja          (#FFD2A1)
      M:  < 3,700K   → rojo             (#FFCC6F)
    """
    if teff is None or math.isnan(teff):
        return (255, 255, 255)  # Default blanco

    # Clamping a rango físico razonable
    teff = max(1000.0, min(40000.0, float(teff)))

    # Algoritmo de Ballesteros (2012) — aproximación analítica
    # Rojo
    if teff <= 6600:
        r = 255
    else:
        x = (teff - 6000) / 10000.0
        r = int(329.698727 * (x ** -0.1332047592))
        r = max(0, min(255, r))

    # Verde
    if teff <= 6600:
        g = int(99.4708025861 * math.log(teff / 100) - 161.1195681661)
    else:
        x = (teff - 6000) / 10000.0
        g = int(288.1221695283 * (x ** -0.0755148492))
    g = max(0, min(255, g))

    # Azul
    if teff >= 6600:
        b = 255
    elif teff <= 1900:
        b = 0
    else:
        b = int(138.5177312231 * math.log(teff / 100 - 10) - 305.0447927307)
    b = max(0, min(255, b))

    return (r, g, b)


def spectral_type_to_teff(sp_type: str) -> float:
    """
    Convierte tipo espectral (e.g. 'G2V', 'K5III') a temperatura efectiva
    aproximada usando tabla de Morgan-Keenan.
    """
    TEFF_MAP = {
        "O": 35000, "B": 20000, "A": 8500,
        "F": 6500,  "G": 5500,  "K": 4200, "M": 3300,
    }
    if not isinstance(sp_type, str) or len(sp_type) == 0:
        return 5778.0  # Temperatura solar por defecto

    first_char = sp_type.strip().upper()[0]
    return TEFF_MAP.get(first_char, 5778.0)


# ──────────────────────────────────────────────────────────────────
# MÓDULO 4: MAGNITUD → TAMAÑO DE PUNTO
# ──────────────────────────────────────────────────────────────────

def magnitude_to_size(magnitude: float,
                      min_size: float = 0.5,
                      max_size: float = 4.0) -> float:
    """
    Convierte magnitud aparente (menor = más brillante) a tamaño
    de punto en la visualización.

    Escala logarítmica inversa: m=-1.5 (Sirio) → 4.0px
                                 m=15   (límite) → 0.5px
    """
    if magnitude is None or math.isnan(magnitude):
        return 1.0

    # Magnitud visual típica: -1.5 (Sirio) a 15 (límite G Gaia)
    mag_min, mag_max = -1.5, 15.0
    normalized = (magnitude - mag_min) / (mag_max - mag_min)
    normalized = max(0.0, min(1.0, normalized))

    # Invertir: estrellas brillantes (mag baja) → puntos grandes
    size = max_size - normalized * (max_size - min_size)
    return round(size, 2)


# ──────────────────────────────────────────────────────────────────
# MÓDULO 5: PROCESAMIENTO PRINCIPAL
# ──────────────────────────────────────────────────────────────────

def process_hyg_dataframe(df: pd.DataFrame) -> list:
    """
    Procesa el DataFrame HYG y devuelve lista de dicts limpios.
    Columnas HYG relevantes:
      x, y, z   → coords cartesianas en parsecs (ya calculadas!)
      mag        → magnitud visual aparente
      spect      → tipo espectral (e.g. 'G2V')
      proper     → nombre propio (e.g. 'Sol', 'Sirio')
      dist       → distancia en parsecs
    """
    print("[PROCESO] Procesando catálogo HYG...")

    # Filtrar filas con coordenadas válidas
    df = df.dropna(subset=["x", "y", "z"])
    df = df[df["dist"] > 0]
    df = df[df["dist"] <= CONFIG["MAX_DISTANCE_PC"]]

    # Ordenar por magnitud (más brillantes primero) y limitar
    df = df.sort_values("mag", na_position="last")
    df = df.head(CONFIG["MAX_STARS"])

    stars = []
    print(f"[PROCESO] Convirtiendo {len(df):,} estrellas...")

    for _, row in tqdm(df.iterrows(), total=len(df), desc="Procesando"):
        # Temperatura: del tipo espectral si no hay teff directa
        teff = row.get("teff", None)
        if pd.isna(teff) if teff is not None else True:
            teff = spectral_type_to_teff(str(row.get("spect", "")))

        r, g, b = teff_to_rgb(teff)
        size    = magnitude_to_size(row.get("mag", 8.0))

        # Normalizar nombre
        name = row.get("proper", None)
        name = str(name).strip() if name and not pd.isna(name) else None

        # ESCALA: dividimos coords por 100 para three.js
        # → 1 unidad Three.js = 100 parsecs
        scale = 0.01
        star = {
            "x": round(float(row["x"]) * scale, 4),
            "y": round(float(row["z"]) * scale, 4),  # Z galáctico → Y en Three.js
            "z": round(float(row["y"]) * scale, 4),
            # Color como hex compacto (ahorra espacio en JSON)
            "c": (r << 16) | (g << 8) | b,
            "s": size,
        }
        if name:
            star["n"] = name

        stars.append(star)

    return stars


def process_gaia_dataframe(df: pd.DataFrame) -> list:
    """
    Procesa el DataFrame de Gaia DR3 y devuelve lista de dicts limpios.
    """
    print("[PROCESO] Procesando datos Gaia DR3...")

    # Calcular distancias desde paralaje
    df = df.dropna(subset=["parallax"])
    df = df[df["parallax"] > 0]
    df["distance_pc"] = parallax_to_distance(df["parallax"].values)
    df = df[df["distance_pc"] <= CONFIG["MAX_DISTANCE_PC"]]
    df = df.sort_values("phot_g_mean_mag", na_position="last")
    df = df.head(CONFIG["MAX_STARS"])

    # Convertir a cartesianas
    x, y, z = equatorial_to_cartesian(
        df["ra"].values,
        df["dec"].values,
        df["distance_pc"].values
    )
    df = df.assign(cart_x=x, cart_y=y, cart_z=z)
    df = df.dropna(subset=["cart_x", "cart_y", "cart_z"])

    stars = []
    scale = 0.01  # 1 unidad = 100 pc

    for _, row in tqdm(df.iterrows(), total=len(df), desc="Procesando"):
        teff = row.get("teff_gspphot", None)
        if pd.isna(teff):
            teff = 5778.0

        r, g, b = teff_to_rgb(float(teff))
        size    = magnitude_to_size(row.get("phot_g_mean_mag", 10.0))

        star = {
            "x": round(float(row["cart_x"]) * scale, 4),
            "y": round(float(row["cart_z"]) * scale, 4),
            "z": round(float(row["cart_y"]) * scale, 4),
            "c": (r << 16) | (g << 8) | b,
            "s": size,
        }
        stars.append(star)

    return stars


# ──────────────────────────────────────────────────────────────────
# MÓDULO 6: EXPORTACIÓN
# ──────────────────────────────────────────────────────────────────

def export_json(stars: list, filepath: str):
    """
    Exporta el catálogo a JSON compacto.
    Formato: { "count": N, "stars": [{x,y,z,c,s}, ...] }
    """
    print(f"[EXPORT] Escribiendo {filepath} ({len(stars):,} estrellas)...")
    payload = {
        "version": "1.0",
        "source": "HYG-v3 / Gaia DR3",
        "count": len(stars),
        "scale": "1 unit = 100 parsecs",
        "fields": "x,y,z: coordenadas | c: color RGB packed int | s: tamaño | n: nombre (opcional)",
        "stars": stars,
    }
    with open(filepath, "w", encoding="utf-8") as f:
        json.dump(payload, f, separators=(",", ":"))  # Sin espacios → compacto
    size_mb = os.path.getsize(filepath) / 1_048_576
    print(f"[EXPORT] ✓ {filepath} exportado ({size_mb:.1f} MB)")


def export_binary(stars: list, filepath: str):
    """
    Exporta a formato binario Float32Array para carga ultra-rápida.
    Layout: [x, y, z, r, g, b, size] por estrella → 7 floats × 4 bytes = 28 bytes/estrella

    Para cargarlo en JS:
        const buf = await fetch('stars.bin').then(r => r.arrayBuffer());
        const data = new Float32Array(buf);
        // data[i*7+0] = x, [i*7+1] = y, ... [i*7+6] = size
    """
    print(f"[EXPORT] Escribiendo binario {filepath}...")
    with open(filepath, "wb") as f:
        # Header: número de estrellas (uint32)
        f.write(struct.pack("<I", len(stars)))
        for s in tqdm(stars, desc="Binario"):
            c = s["c"]
            r = ((c >> 16) & 0xFF) / 255.0
            g = ((c >> 8)  & 0xFF) / 255.0
            b = ( c        & 0xFF) / 255.0
            f.write(struct.pack("<fffffff",
                s["x"], s["y"], s["z"], r, g, b, s["s"]))

    size_mb = os.path.getsize(filepath) / 1_048_576
    print(f"[EXPORT] ✓ {filepath} exportado ({size_mb:.1f} MB)")


# ──────────────────────────────────────────────────────────────────
# PUNTO DE ENTRADA
# ──────────────────────────────────────────────────────────────────

def main():
    print("=" * 60)
    print("  MILKY WAY STAR PROCESSOR — Iniciando pipeline")
    print("=" * 60)

    stars = None

    # Intentar Gaia primero (mayor calidad, requiere astroquery + red)
    if CONFIG["USE_GAIA"]:
        gaia_df = query_gaia_dr3(limit=CONFIG["MAX_STARS"] * 2)
        if gaia_df is not None and len(gaia_df) > 1000:
            stars = process_gaia_dataframe(gaia_df)

    # Fallback: catálogo HYG (siempre disponible, muy rápido)
    if stars is None:
        print("[INFO] Usando catálogo HYG v3 como fuente de datos.")
        hyg_df = download_hyg_catalog()
        stars  = process_hyg_dataframe(hyg_df)

    if not stars:
        print("[ERROR] No se pudieron procesar estrellas.")
        sys.exit(1)

    print(f"\n[RESUMEN] Total estrellas procesadas: {len(stars):,}")

    # Exportar JSON (siempre)
    export_json(stars, CONFIG["OUTPUT_JSON"])

    # Exportar binario (opcional, para carga más rápida)
    if CONFIG["EXPORT_BINARY"]:
        export_binary(stars, CONFIG["OUTPUT_BINARY"])

    print("\n[OK] Pipeline completado. Archivos generados:")
    print(f"     → {CONFIG['OUTPUT_JSON']}")
    if CONFIG["EXPORT_BINARY"]:
        print(f"     → {CONFIG['OUTPUT_BINARY']}")
    print("\nSiguiente paso: abre index.html en tu servidor local.")
    print("  python -m http.server 8000")
    print("  http://localhost:8000")


if __name__ == "__main__":
    main()
