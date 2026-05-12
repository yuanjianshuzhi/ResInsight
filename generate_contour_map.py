#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
等高线图生成工具 - 支持多种聚合类型与几何相交面积加权投影（性能优化版）
新增含水饱和度垂直剖面图生成功能

用法:
    python generate_contour_map.py -help                          # 显示此帮助信息
    python generate_contour_map.py <case_path> help              # 显示该 case 的总时间步数
    python generate_contour_map.py <case_path> <agg_type> <time_step>  # 生成等高线图

聚合类型 (完整支持):
    oil_column          - Oil Column: Σ(SOIL * NTG * PORO * dZ)
    gas_column          - Gas Column: Σ(SGAS * NTG * PORO * dZ)
    hydrocarbon_column  - Hydrocarbon Column: Σ((SOIL+SGAS) * NTG * PORO * dZ)
    mobile_oil_column   - Mobile Oil Column: Σ((SOIL - SOWCR) * NTG * PORO * dZ)  (默认 SOWCR=0.2)
    mobile_gas_column   - Mobile Gas Column: Σ((SGAS - SGCR) * NTG * PORO * dZ)   (默认 SGCR=0.05)
    mobile_hydrocarbon_column - Mobile Hydrocarbon Column: Σ((SOIL - SOWCR + SGAS - SGCR) * NTG * PORO * dZ)
    arithmetic_mean     - 算术平均 (直接使用 SOIL)
    harmonic_mean       - 调和平均
    geometric_mean      - 几何平均
    volume_weighted_sum - 体积加权和 (等同于 oil_column)
    sum                 - 简单求和 (直接对 SOIL 求和，无体积加权)
    top_value           - 取垂直方向顶层值
    min_value           - 取垂直方向最小值
    max_value           - 取垂直方向最大值

含水饱和度垂直剖面图模式:
    python generate_contour_map.py <case_path> water_profiles <x_ratio> <y_ratio> <time_step>
    说明: x_ratio 和 y_ratio 必须有且仅有一个为 0.00，另一个在 [0,1] 之间
          x_ratio>0 时沿 X 轴固定位置切出 YoZ 平面
          y_ratio>0 时沿 Y 轴固定位置切出 XoZ 平面

时间步:
    0-based 索引，支持负数（-1 表示最后一步）

示例:
    # 等高线图
    python generate_contour_map.py C:\\models\\CASE oil_column 86
    python generate_contour_map.py C:\\models\\CASE mobile_oil_column 86

    # 含水剖面图
    python generate_contour_map.py C:\\models\\CASE water_profiles 0.00 0.15 86
    python generate_contour_map.py C:\\models\\CASE water_profiles 0.33 0.00 99
"""

import sys
import os
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.tri import Triangulation
import ecl_data_io as eclio
import warnings
from shapely.geometry import Polygon, box
from rtree import index
from tqdm import tqdm

warnings.filterwarnings("ignore")

# ======================== 常量定义 ========================
COORD_PAD_RATIO = 0.02
ZCORN_BOT_IDX = slice(0, 4)
ZCORN_TOP_IDX = slice(4, 8)
DEFAULT_SOWCR = 0.2
DEFAULT_SGCR = 0.05
DEFAULT_RESOLUTION = 150
DEFAULT_CONTOUR_LEVELS = 8
SAVE_DPI = 300
PLOT_DPI = 150
PLOT_FIGSIZE = (12, 10)

# ======================== 帮助信息 ========================
def print_help():
    print(__doc__)

# ======================== 网格维度读取 ========================
def get_grid_dimensions(egrid_file):
    for kw, data in eclio.read(egrid_file):
        kw_clean = kw.strip().upper()
        if kw_clean == "GRIDHEAD" and len(data) >= 4:
            nx = int(data[1]); ny = int(data[2]); nz = int(data[3])
            if nx > 0 and ny > 0 and nz > 0:
                return nx, ny, nz
        elif kw_clean == "FILEHEAD" and len(data) >= 3:
            nx = int(data[0]); ny = int(data[1]); nz = int(data[2])
            if nx > 0 and ny > 0 and nz > 0:
                return nx, ny, nz
    raise ValueError("无法读取网格维度")

# ======================== 读取 COORD, ZCORN, ACTNUM ========================
def read_grid_data(egrid_file):
    coord = zcorn = actnum = None
    for kw, data in eclio.read(egrid_file):
        kw_clean = kw.strip().upper()
        if kw_clean == "COORD":
            coord = np.array(data, dtype=np.float64)
        elif kw_clean == "ZCORN":
            zcorn = np.array(data, dtype=np.float64)
        elif kw_clean == "ACTNUM":
            actnum = np.array(data, dtype=np.int32)
    if coord is None or zcorn is None:
        raise ValueError("EGRID 缺少 COORD 或 ZCORN")
    return coord, zcorn, actnum

# ======================== 从 .INIT 文件读取静态属性 ========================
def read_static_properties(init_file):
    props = {}
    if not init_file or not os.path.exists(init_file):
        return props
    try:
        for kw, data in eclio.read(init_file):
            kw_clean = kw.strip().upper()
            if kw_clean in ["PORO", "NTG"]:
                props[kw_clean] = np.array(data, dtype=np.float64)
                print(f"从 INIT 文件读取到 {kw_clean}")
    except Exception as e:
        print(f"警告: 读取 INIT 文件 {init_file} 时出错: {e}")
    return props

# ======================== 获取每个单元的顶面多边形 ========================
def get_cell_polygons_c_order(coord, nx, ny, nz, only_top_layer=False):
    pillars = coord.reshape((ny + 1, nx + 1, 6), order='C')
    top_xy = pillars[:, :, :2]
    layers = [0] if only_top_layer else range(nz)
    polygons = np.empty((len(layers), ny, nx), dtype=object)
    i_indices = np.arange(nx)
    j_indices = np.arange(ny)
    j_grid, i_grid = np.meshgrid(j_indices, i_indices, indexing='ij')
    p0 = top_xy[j_grid, i_grid]
    p1 = top_xy[j_grid, i_grid + 1]
    p2 = top_xy[j_grid + 1, i_grid + 1]
    p3 = top_xy[j_grid + 1, i_grid]
    for k_idx, k in enumerate(layers):
        for j in range(ny):
            for i in range(nx):
                poly = Polygon([p0[j,i], p1[j,i], p2[j,i], p3[j,i]])
                if not poly.is_valid:
                    poly = poly.buffer(0)
                polygons[k_idx, j, i] = poly
    if only_top_layer:
        full_polygons = np.empty((nz, ny, nx), dtype=object)
        full_polygons[0] = polygons[0]
        for k in range(1, nz):
            full_polygons[k] = None
        return full_polygons
    return polygons

# ======================== 计算单元厚度 ========================
def compute_cell_thickness(zcorn, nx, ny, nz):
    if len(zcorn) == nz * (ny+1) * (nx+1) * 8:
        z = zcorn.reshape((nz, ny+1, nx+1, 8), order='C')
        bot_mean = np.mean(z[..., ZCORN_BOT_IDX], axis=-1)
        top_mean = np.mean(z[..., ZCORN_TOP_IDX], axis=-1)
        thickness = np.abs(top_mean[:, :-1, :-1] - bot_mean[:, :-1, :-1])
    else:
        z = zcorn.reshape((nz, ny, nx, 8), order='C')
        bot_mean = np.mean(z[..., ZCORN_BOT_IDX], axis=-1)
        top_mean = np.mean(z[..., ZCORN_TOP_IDX], axis=-1)
        thickness = np.abs(top_mean - bot_mean)
    return thickness

# ======================== 属性数据读取 ========================
def read_property_at_step(unrst_file, property_key, target_step):
    current_step = -1
    for kw, data in eclio.read(unrst_file):
        kw_clean = kw.strip().upper()
        if kw_clean == "SEQNUM":
            current_step += 1
            if current_step > target_step:
                break
        elif current_step == target_step and kw_clean == property_key.upper():
            return np.array(data, dtype=np.float64).flatten()
    raise ValueError(f"未找到时间步 {target_step} 的属性 {property_key}")

def get_total_steps(unrst_file):
    steps = 0
    for kw, _ in eclio.read(unrst_file):
        if kw.strip().upper() == "SEQNUM":
            steps += 1
    return steps

def map_property_to_full_c_order(prop_data, actnum, nx, ny, nz):
    total = nx * ny * nz
    act_flat = actnum.reshape((total,), order='C')
    active_indices = np.where(act_flat == 1)[0]
    full = np.full(total, np.nan, dtype=np.float64)
    prop_arr = prop_data.flatten()
    if prop_arr.size == active_indices.size:
        full[active_indices] = prop_arr
    elif prop_arr.size == total:
        full[:] = prop_arr
    else:
        full[:min(prop_arr.size, total)] = prop_arr[:min(prop_arr.size, total)]
    return full.reshape((nz, ny, nx), order='C')

# ======================== 构建投影网格（等高线图用） ========================
def build_projection_grid_intersection(cell_polygons, cell_values, cell_weights,
                                       actnum_3d, resolution=DEFAULT_RESOLUTION):
    active = (actnum_3d == 1) & np.isfinite(cell_values)
    if not np.any(active):
        raise ValueError("没有活跃的有效单元")
    valid_mask = active & (cell_polygons != None)
    all_polys = cell_polygons[valid_mask]
    valid_indices = np.argwhere(valid_mask)
    valid_values = cell_values[valid_mask]
    valid_weights = cell_weights[valid_mask]
    bounds = [poly.bounds for poly in all_polys]
    x_min = min(b[0] for b in bounds)
    y_min = min(b[1] for b in bounds)
    x_max = max(b[2] for b in bounds)
    y_max = max(b[3] for b in bounds)
    x_pad = (x_max - x_min) * COORD_PAD_RATIO
    y_pad = (y_max - y_min) * COORD_PAD_RATIO
    x_edges = np.linspace(x_min - x_pad, x_max + x_pad, resolution + 1)
    y_edges = np.linspace(y_min - y_pad, y_max + y_pad, resolution + 1)
    grid_cells = []
    grid_bounds = []
    for j in range(resolution):
        for i in range(resolution):
            rect = box(x_edges[i], y_edges[j], x_edges[i+1], y_edges[j+1])
            grid_cells.append(rect)
            grid_bounds.append(rect.bounds)
    idx = index.Index()
    for grid_id, b in enumerate(grid_bounds):
        idx.insert(grid_id, b)
    sum_weighted = np.zeros((resolution, resolution), dtype=np.float64)
    sum_weights = np.zeros((resolution, resolution), dtype=np.float64)
    pbar = tqdm(enumerate(zip(all_polys, valid_values, valid_weights)), 
                total=len(all_polys), desc="处理活跃单元")
    for idx_poly, (poly, val, w) in pbar:
        possible_grid_ids = list(idx.intersection(poly.bounds))
        if not possible_grid_ids:
            continue
        for grid_id in possible_grid_ids:
            cell_rect = grid_cells[grid_id]
            if poly.intersects(cell_rect):
                area = poly.intersection(cell_rect).area
                if area > 0:
                    jj = grid_id // resolution
                    ii = grid_id % resolution
                    sum_weighted[jj, ii] += val * w * area
                    sum_weights[jj, ii] += w * area
    cell_agg = np.divide(sum_weighted, sum_weights,
                         out=np.full_like(sum_weighted, np.nan),
                         where=sum_weights > 0)
    vertex_values = np.full((resolution + 1, resolution + 1), np.nan)
    shifts = [(-1, -1), (-1, 0), (0, -1), (0, 0)]
    for dj, di in shifts:
        shifted = np.roll(np.roll(cell_agg, dj, axis=0), di, axis=1)
        mask = (np.arange(resolution+1)[:-1] + dj >= 0)[:, None] & \
               (np.arange(resolution+1)[:-1] + di >= 0)[None, :]
        vertex_values[1:resolution+1, 1:resolution+1][mask] = np.nanmean(
            [vertex_values[1:resolution+1, 1:resolution+1][mask], shifted[mask]], axis=0
        )
    X, Y = np.meshgrid(x_edges, y_edges)
    return X, Y, vertex_values

# ======================== 绘制等高线 ========================
def plot_contour_from_vertex_grid(X, Y, vertex_values, title, output_file,
                                  contour_levels=DEFAULT_CONTOUR_LEVELS, vmin=None, vmax=None, swap_xy=False):
    ny, nx = vertex_values.shape
    triangles = []
    for j in range(ny - 1):
        for i in range(nx - 1):
            idx00 = j * nx + i
            idx10 = j * nx + (i + 1)
            idx01 = (j + 1) * nx + i
            idx11 = (j + 1) * nx + (i + 1)
            v1, v2, v3 = vertex_values[j, i], vertex_values[j, i+1], vertex_values[j+1, i]
            if not (np.isnan(v1) or np.isnan(v2) or np.isnan(v3)):
                triangles.append([idx00, idx10, idx01])
            v1, v2, v3 = vertex_values[j, i+1], vertex_values[j+1, i+1], vertex_values[j+1, i]
            if not (np.isnan(v1) or np.isnan(v2) or np.isnan(v3)):
                triangles.append([idx10, idx11, idx01])
    if not triangles:
        raise ValueError("没有有效的三角形数据")
    triangles = np.array(triangles)
    x_flat = X.flatten()
    y_flat = Y.flatten()
    z_flat = vertex_values.flatten()
    triang = Triangulation(x_flat, y_flat, triangles)
    valid_z = z_flat[~np.isnan(z_flat)]
    if vmin is None:
        vmin = np.percentile(valid_z, 0.5)
    if vmax is None:
        vmax = np.percentile(valid_z, 99.5)
    plt.figure(figsize=PLOT_FIGSIZE, dpi=PLOT_DPI)
    contourf = plt.tricontourf(triang, z_flat, levels=contour_levels,
                               cmap='jet', vmin=vmin, vmax=vmax)
    contour = plt.tricontour(triang, z_flat, levels=contour_levels,
                             colors='black', linewidths=0.5)
    plt.clabel(contour, inline=True, fontsize=8, fmt='%.3f')
    plt.colorbar(contourf, label='Value')
    plt.title(title, fontsize=12)
    if swap_xy:
        plt.xlabel('Y Coordinate (m)', fontsize=10)
        plt.ylabel('X Coordinate (m)', fontsize=10)
    else:
        plt.xlabel('X Coordinate (m)', fontsize=10)
        plt.ylabel('Y Coordinate (m)', fontsize=10)
    plt.axis('equal')
    plt.tight_layout()
    if output_file:
        plt.savefig(output_file, dpi=SAVE_DPI, bbox_inches='tight', facecolor='white')
        print(f"✅ 等高线图已保存: {output_file}")
    else:
        plt.show()
    plt.close()

# ======================== 辅助函数：沿垂直方向计算统计量 ========================
def vertical_mean(values_3d, thickness=None, weight_type='arithmetic'):
    nz, ny, nx = values_3d.shape
    if weight_type == 'arithmetic':
        if thickness is None:
            return np.nanmean(values_3d, axis=0)
        else:
            weights = thickness
            sum_w = np.nansum(weights, axis=0)
            sum_wv = np.nansum(values_3d * weights, axis=0)
            return np.divide(sum_wv, sum_w, out=np.full((ny, nx), np.nan), where=sum_w > 0)
    elif weight_type == 'harmonic':
        with np.errstate(divide='ignore', invalid='ignore'):
            inv = 1.0 / values_3d
            sum_inv = np.nansum(inv, axis=0)
            n_valid = np.sum(~np.isnan(values_3d), axis=0)
            return np.divide(n_valid, sum_inv, out=np.full((ny, nx), np.nan), where=sum_inv > 0)
    elif weight_type == 'geometric':
        log_vals = np.log(values_3d)
        sum_log = np.nansum(log_vals, axis=0)
        n_valid = np.sum(~np.isnan(values_3d), axis=0)
        return np.exp(np.divide(sum_log, n_valid, out=np.full((ny, nx), np.nan), where=n_valid > 0))
    elif weight_type == 'volume_weighted_sum':
        if thickness is None:
            return np.nansum(values_3d, axis=0)
        else:
            return np.nansum(values_3d * thickness, axis=0)
    elif weight_type == 'sum':
        return np.nansum(values_3d, axis=0)
    elif weight_type == 'top':
        return values_3d[0, :, :]
    elif weight_type == 'min':
        return np.nanmin(values_3d, axis=0)
    elif weight_type == 'max':
        return np.nanmax(values_3d, axis=0)
    else:
        raise ValueError(f"未知的垂直聚合类型: {weight_type}")

def project_plane_values(plane_polygons, plane_values, resolution=DEFAULT_RESOLUTION):
    bounds = [poly.bounds for poly in plane_polygons if poly is not None]
    x_min = min(b[0] for b in bounds)
    y_min = min(b[1] for b in bounds)
    x_max = max(b[2] for b in bounds)
    y_max = max(b[3] for b in bounds)
    x_pad = (x_max - x_min) * COORD_PAD_RATIO
    y_pad = (y_max - y_min) * COORD_PAD_RATIO
    x_edges = np.linspace(x_min - x_pad, x_max + x_pad, resolution + 1)
    y_edges = np.linspace(y_min - y_pad, y_max + y_pad, resolution + 1)
    grid_cells = []
    grid_bounds = []
    for j in range(resolution):
        for i in range(resolution):
            rect = box(x_edges[i], y_edges[j], x_edges[i+1], y_edges[j+1])
            grid_cells.append(rect)
            grid_bounds.append(rect.bounds)
    idx = index.Index()
    for grid_id, b in enumerate(grid_bounds):
        idx.insert(grid_id, b)
    sum_vals = np.zeros((resolution, resolution), dtype=np.float64)
    count = np.zeros((resolution, resolution), dtype=np.float64)
    pbar = tqdm(zip(plane_polygons, plane_values), total=len(plane_polygons), desc="投影平面值")
    for poly, val in pbar:
        if np.isnan(val) or poly is None:
            continue
        possible_grid_ids = list(idx.intersection(poly.bounds))
        for grid_id in possible_grid_ids:
            cell_rect = grid_cells[grid_id]
            if poly.intersects(cell_rect):
                area = poly.intersection(cell_rect).area
                if area > 0:
                    jj = grid_id // resolution
                    ii = grid_id % resolution
                    sum_vals[jj, ii] += val * area
                    count[jj, ii] += area
    cell_agg = np.divide(sum_vals, count, out=np.full_like(sum_vals, np.nan), where=count > 0)
    vertex_values = np.full((resolution + 1, resolution + 1), np.nan)
    shifts = [(-1, -1), (-1, 0), (0, -1), (0, 0)]
    for dj, di in shifts:
        shifted = np.roll(np.roll(cell_agg, dj, axis=0), di, axis=1)
        mask = (np.arange(resolution+1)[:-1] + dj >= 0)[:, None] & \
               (np.arange(resolution+1)[:-1] + di >= 0)[None, :]
        vertex_values[1:resolution+1, 1:resolution+1][mask] = np.nanmean(
            [vertex_values[1:resolution+1, 1:resolution+1][mask], shifted[mask]], axis=0
        )
    X, Y = np.meshgrid(x_edges, y_edges)
    return X, Y, vertex_values

# ======================== 新增：含水饱和度垂直剖面图 ========================
def generate_water_profile(case_path, x_ratio, y_ratio, time_step):
    GRID_FILE = case_path + ".EGRID"
    UNRST_FILE = case_path + ".UNRST"
    INIT_FILE = case_path + ".INIT"

    if not os.path.exists(GRID_FILE) or not os.path.exists(UNRST_FILE):
        print(f"错误: 网格或重启文件不存在")
        sys.exit(1)

    # 读取原始声明的维度（可能不准确，仅用于参考）
    nx_decl, ny_decl, nz_decl = get_grid_dimensions(GRID_FILE)
    print(f"声明网格维度: {nx_decl} x {ny_decl} x {nz_decl}")

    coord, zcorn, actnum = read_grid_data(GRID_FILE)
    if actnum is None:
        actnum = np.ones(nx_decl * ny_decl * nz_decl, dtype=np.int32)

    # ---------- 从 ZCORN 推断真实维度 ----------
    # 判断 ZCORN 布局: 8 * nz * (ny+1) * (nx+1) 还是 8 * nz * ny * nx
    total_zcorn = len(zcorn)
    # 尝试第一种布局 (带 +1)
    nz_est = nz_decl
    ny_est = ny_decl
    nx_est = nx_decl
    if total_zcorn == 8 * nz_est * (ny_est + 1) * (nx_est + 1):
        z = zcorn.reshape((nz_est, ny_est + 1, nx_est + 1, 8), order='C')
        bot_mean = np.mean(z[..., ZCORN_BOT_IDX], axis=-1)
        top_mean = np.mean(z[..., ZCORN_TOP_IDX], axis=-1)
        z_center = (top_mean[:, :-1, :-1] + bot_mean[:, :-1, :-1]) / 2.0   # (nz, ny, nx)
    elif total_zcorn == 8 * nz_est * ny_est * nx_est:
        z = zcorn.reshape((nz_est, ny_est, nx_est, 8), order='C')
        bot_mean = np.mean(z[..., ZCORN_BOT_IDX], axis=-1)
        top_mean = np.mean(z[..., ZCORN_TOP_IDX], axis=-1)
        z_center = (top_mean + bot_mean) / 2.0
    else:
        # 尝试从数组长度反向推断
        # 可能的因子: total_zcorn / 8 必须能分解为 nz * (ny+1) * (nx+1) 或 nz * ny * nx
        vol = total_zcorn // 8
        # 简单处理: 假设与声明一致
        raise ValueError(f"无法解析 ZCORN 长度 {total_zcorn}，与声明维度不匹配")

    # 获取实际维度
    nz, ny, nx = z_center.shape
    if (nx, ny, nz) != (nx_decl, ny_decl, nz_decl):
        print(f"实际网格维度 (从 ZCORN 推断): {nx} x {ny} x {nz}")
        # 重新调整 actnum 形状
        actnum_flat = actnum.flatten()
        expected_size = nx * ny * nz
        if len(actnum_flat) >= expected_size:
            actnum_3d = actnum_flat[:expected_size].reshape((nz, ny, nx), order='C')
        else:
            # 补全为活跃
            actnum_3d = np.ones((nz, ny, nx), dtype=np.int32)
    else:
        actnum_3d = actnum.reshape((nz, ny, nx), order='C')

    # ---------- 重新计算单元中心坐标 (使用实际维度) ----------
    # 从 COORD 构建 pillars
    # COORD 长度应为 6 * (nx+1) * (ny+1)
    expected_coord_len = 6 * (nx + 1) * (ny + 1)
    if len(coord) < expected_coord_len:
        # 如果 COORD 长度不足，尝试按声明维度重新采样（极少见）
        print(f"警告: COORD 长度 {len(coord)} 小于预期 {expected_coord_len}，将使用声明维度重建")
        # 回退到声明维度
        nx, ny = nx_decl, ny_decl
        expected_coord_len = 6 * (nx + 1) * (ny + 1)
    pillars = coord[:expected_coord_len].reshape((ny + 1, nx + 1, 6), order='C')
    x_coords = pillars[:, :, 0]
    y_coords = pillars[:, :, 1]

    x_center = np.zeros((ny, nx))
    y_center = np.zeros((ny, nx))
    for j in range(ny):
        for i in range(nx):
            x_center[j, i] = (x_coords[j, i] + x_coords[j, i+1] +
                              x_coords[j+1, i] + x_coords[j+1, i+1]) / 4.0
            y_center[j, i] = (y_coords[j, i] + y_coords[j, i+1] +
                              y_coords[j+1, i] + y_coords[j+1, i+1]) / 4.0

    # Bounding box
    xmin, xmax = np.min(x_center), np.max(x_center)
    ymin, ymax = np.min(y_center), np.max(y_center)

    # 确定剖面方向
    if x_ratio != 0.0 and y_ratio == 0.0:
        cut_axis = 'X'
        cut_pos = xmin + x_ratio * (xmax - xmin)
        x_center_i = np.mean(x_center, axis=0)   # (nx,)
        i_index = np.argmin(np.abs(x_center_i - cut_pos))
        print(f"X 剖面: 比率 {x_ratio} -> 坐标 {cut_pos:.2f}, 选取 I = {i_index}")
        profile_vals = np.full((nz, ny), np.nan)
        profile_x = np.zeros((nz, ny))   # 横坐标：Y
        profile_z = np.zeros((nz, ny))   # 纵坐标：Z
        for k in range(nz):
            for j in range(ny):
                profile_x[k, j] = y_center[j, i_index]
                profile_z[k, j] = z_center[k, j, i_index]
        xlabel = 'Y Coordinate (m)'
        title_axis = f'X = {cut_pos:.2f} m (I={i_index})'
    elif y_ratio != 0.0 and x_ratio == 0.0:
        cut_axis = 'Y'
        cut_pos = ymin + y_ratio * (ymax - ymin)
        y_center_j = np.mean(y_center, axis=1)   # (ny,)
        j_index = np.argmin(np.abs(y_center_j - cut_pos))
        print(f"Y 剖面: 比率 {y_ratio} -> 坐标 {cut_pos:.2f}, 选取 J = {j_index}")
        profile_vals = np.full((nz, nx), np.nan)
        profile_x = np.zeros((nz, nx))   # 横坐标：X
        profile_z = np.zeros((nz, nx))   # 纵坐标：Z
        for k in range(nz):
            for i in range(nx):
                profile_x[k, i] = x_center[j_index, i]
                profile_z[k, i] = z_center[k, j_index, i]
        xlabel = 'X Coordinate (m)'
        title_axis = f'Y = {cut_pos:.2f} m (J={j_index})'
    else:
        raise ValueError("无效的剖面比率")

    # 读取 SWAT
    total_steps = get_total_steps(UNRST_FILE)
    if time_step < 0:
        time_step = total_steps + time_step
    if time_step < 0 or time_step >= total_steps:
        print(f"错误: 时间步 {time_step} 超出范围 (0-{total_steps-1})")
        sys.exit(1)

    swat_raw = read_property_at_step(UNRST_FILE, "SWAT", time_step)
    # 映射到完整网格 (使用实际维度)
    total_cells = nx * ny * nz
    act_flat = actnum_3d.flatten(order='C')
    active_indices = np.where(act_flat == 1)[0]
    full = np.full(total_cells, np.nan, dtype=np.float64)
    swat_arr = swat_raw.flatten()
    if swat_arr.size == active_indices.size:
        full[active_indices] = swat_arr
    elif swat_arr.size == total_cells:
        full[:] = swat_arr
    else:
        full[:min(swat_arr.size, total_cells)] = swat_arr[:min(swat_arr.size, total_cells)]
    swat_3d = full.reshape((nz, ny, nx), order='C')
    swat_3d = np.clip(swat_3d, 0.0, 1.0)
    swat_3d[actnum_3d == 0] = np.nan

    # 填充剖面值
    if cut_axis == 'X':
        for k in range(nz):
            for j in range(ny):
                profile_vals[k, j] = swat_3d[k, j, i_index]
    else:
        for k in range(nz):
            for i in range(nx):
                profile_vals[k, i] = swat_3d[k, j_index, i]

    # 绘图
    plt.figure(figsize=(10, 8), dpi=150)
    mesh = plt.pcolormesh(profile_x, profile_z, profile_vals,
                          shading='auto', cmap='jet_r', vmin=0, vmax=1)
    plt.colorbar(mesh, label='Water Saturation (SWAT)')
    plt.xlabel(xlabel, fontsize=10)
    plt.ylabel('Depth (Z) (m)', fontsize=10)
    plt.title(f"Water Saturation Profile - Time Step {time_step}\n{title_axis}", fontsize=12)
    plt.gca().invert_yaxis()
    plt.tight_layout()

    out_file = f"{case_path}_water_profile_{cut_axis}_{max(x_ratio, y_ratio):.2f}_step_{time_step}.png"
    plt.savefig(out_file, dpi=300, bbox_inches='tight', facecolor='white')
    print(f"✅ 含水剖面图已保存: {out_file}")
    plt.close()

# ======================== 主程序 ========================
def main():
    if len(sys.argv) == 2 and sys.argv[1].lower() in ["-help", "--help"]:
        print_help()
        sys.exit(0)

    if len(sys.argv) < 2:
        print_help()
        sys.exit(1)

    case_path = sys.argv[1]

    # 处理 water_profiles 模式
    if len(sys.argv) == 6 and sys.argv[2].lower() == "water_profiles":
        try:
            x_ratio = float(sys.argv[3])
            y_ratio = float(sys.argv[4])
            time_step = int(sys.argv[5])
        except ValueError:
            print("错误: 比率和时间步必须是数值")
            sys.exit(1)

        if (x_ratio == 0.0 and 0.0 <= y_ratio <= 1.0) or (y_ratio == 0.0 and 0.0 <= x_ratio <= 1.0):
            generate_water_profile(case_path, x_ratio, y_ratio, time_step)
        else:
            print("错误: 两个比率必须有且仅有一个为0.00，另一个在0~1之间")
            sys.exit(1)
        return

    # 原有的等高线图模式
    if len(sys.argv) == 3 and sys.argv[2].lower() == "help":
        try:
            UNRST_FILE = case_path + ".UNRST"
            if not os.path.exists(UNRST_FILE):
                print(f"错误: 重启文件不存在 {UNRST_FILE}")
                sys.exit(1)
            total_steps = get_total_steps(UNRST_FILE)
            print(f"总时间步数: {total_steps}")
            sys.exit(0)
        except Exception as e:
            print(f"读取时间步数失败: {e}")
            sys.exit(1)

    if len(sys.argv) != 4:
        print_help()
        sys.exit(1)

    agg_type = sys.argv[2].lower()
    try:
        time_step = int(sys.argv[3])
    except ValueError:
        print("错误: 时间步必须是整数")
        sys.exit(1)

    OUTPUT_FILE = f"{case_path}_{agg_type}_step_{time_step}.png"
    SWAP_XY = False

    # 以下为原有等高线图处理逻辑（完全未变）
    GRID_FILE = case_path + ".EGRID"
    UNRST_FILE = case_path + ".UNRST"
    INIT_FILE = case_path + ".INIT"

    if not os.path.exists(GRID_FILE):
        print(f"错误: 网格文件不存在 {GRID_FILE}")
        sys.exit(1)
    if not os.path.exists(UNRST_FILE):
        print(f"错误: 重启文件不存在 {UNRST_FILE}")
        sys.exit(1)

    nx, ny, nz = get_grid_dimensions(GRID_FILE)
    print(f"网格维度: {nx} x {ny} x {nz}, 总单元数: {nx*ny*nz}")

    coord, zcorn, actnum = read_grid_data(GRID_FILE)
    if actnum is None:
        actnum = np.ones(nx * ny * nz, dtype=np.int32)
    print("已读取 COORD, ZCORN, ACTNUM")

    only_top_layer = (agg_type == "top_value")
    cell_polygons = get_cell_polygons_c_order(coord, nx, ny, nz, only_top_layer=only_top_layer)

    thickness = compute_cell_thickness(zcorn, nx, ny, nz)

    static_props = read_static_properties(INIT_FILE)
    poro_data = static_props.get("PORO")
    ntg_data = static_props.get("NTG")

    if poro_data is not None:
        poro_3d = map_property_to_full_c_order(poro_data, actnum, nx, ny, nz)
        print("已从 INIT 文件加载 PORO")
    else:
        poro_3d = None
        print("警告: INIT 文件中未找到 PORO，将尝试从 UNRST 读取")

    if ntg_data is not None:
        ntg_3d = map_property_to_full_c_order(ntg_data, actnum, nx, ny, nz)
        print("已从 INIT 文件加载 NTG")
    else:
        ntg_3d = None
        print("警告: INIT 文件中未找到 NTG，将默认设为 1.0")

    total_steps = get_total_steps(UNRST_FILE)
    if time_step < 0:
        time_step = total_steps + time_step
    if time_step < 0 or time_step >= total_steps:
        print(f"错误: 时间步 {time_step} 超出范围 (0-{total_steps-1})")
        sys.exit(1)
    print(f"总时间步数: {total_steps}, 目标步: {time_step}")

    need_soil = agg_type in ["oil_column", "hydrocarbon_column", "mobile_oil_column", "mobile_hydrocarbon_column",
                             "arithmetic_mean", "harmonic_mean", "geometric_mean", "volume_weighted_sum", "sum",
                             "top_value", "min_value", "max_value"]
    need_sgas = agg_type in ["gas_column", "hydrocarbon_column", "mobile_gas_column", "mobile_hydrocarbon_column"]
    need_ntg_poro = agg_type in ["oil_column", "gas_column", "hydrocarbon_column", "mobile_oil_column",
                                 "mobile_gas_column", "mobile_hydrocarbon_column", "volume_weighted_sum"]

    soil_3d = None
    sgas_3d = None
    if need_soil:
        soil_raw = read_property_at_step(UNRST_FILE, "SOIL", time_step)
        soil_3d = map_property_to_full_c_order(soil_raw, actnum, nx, ny, nz)
        soil_3d = np.clip(soil_3d, 0.0, 1.0, out=soil_3d)
        soil_3d[soil_3d < 0.0] = np.nan
        soil_3d[soil_3d > 1.0] = np.nan
        print("已读取 SOIL")
    if need_sgas:
        sgas_raw = read_property_at_step(UNRST_FILE, "SGAS", time_step)
        sgas_3d = map_property_to_full_c_order(sgas_raw, actnum, nx, ny, nz)
        sgas_3d = np.clip(sgas_3d, 0.0, 1.0, out=sgas_3d)
        sgas_3d[sgas_3d < 0.0] = np.nan
        sgas_3d[sgas_3d > 1.0] = np.nan
        print("已读取 SGAS")

    if need_ntg_poro and poro_3d is None:
        try:
            poro_raw = read_property_at_step(UNRST_FILE, "PORO", 0)
            poro_3d = map_property_to_full_c_order(poro_raw, actnum, nx, ny, nz)
            print("已从 UNRST 第一个时间步读取 PORO")
        except ValueError:
            raise ValueError("无法获取 PORO 数据，请确保 INIT 或 UNRST 中包含 PORO 关键字")
    if need_ntg_poro and ntg_3d is None:
        ntg_3d = np.ones_like(soil_3d if soil_3d is not None else np.ones((nz, ny, nx)))
        print("NTG 未找到，默认设为 1.0")

    actnum_3d = actnum.reshape((nz, ny, nx), order='C')

    if agg_type in ["arithmetic_mean", "harmonic_mean", "geometric_mean", "top_value", "min_value", "max_value"]:
        if agg_type == "arithmetic_mean":
            plane_val = vertical_mean(soil_3d, weight_type='arithmetic')
        elif agg_type == "harmonic_mean":
            plane_val = vertical_mean(soil_3d, weight_type='harmonic')
        elif agg_type == "geometric_mean":
            plane_val = vertical_mean(soil_3d, weight_type='geometric')
        elif agg_type == "top_value":
            plane_val = vertical_mean(soil_3d, weight_type='top')
        elif agg_type == "min_value":
            plane_val = vertical_mean(soil_3d, weight_type='min')
        elif agg_type == "max_value":
            plane_val = vertical_mean(soil_3d, weight_type='max')
        else:
            raise ValueError("未知类型")

        plane_polygons = [cell_polygons[0, j, i] for j in range(ny) for i in range(nx)]
        plane_values_flat = plane_val.flatten()
        X, Y, vertex_values = project_plane_values(plane_polygons, plane_values_flat, resolution=DEFAULT_RESOLUTION)
        title = f"{agg_type.upper()} - Time Step {time_step}"
        plot_contour_from_vertex_grid(X, Y, vertex_values, title, OUTPUT_FILE,
                                      contour_levels=DEFAULT_CONTOUR_LEVELS, swap_xy=SWAP_XY)
        return

    if agg_type == "oil_column":
        cell_value = soil_3d * ntg_3d * poro_3d * thickness
        weight = np.ones_like(cell_value)
    elif agg_type == "gas_column":
        cell_value = sgas_3d * ntg_3d * poro_3d * thickness
        weight = np.ones_like(cell_value)
    elif agg_type == "hydrocarbon_column":
        cell_value = (soil_3d + sgas_3d) * ntg_3d * poro_3d * thickness
        weight = np.ones_like(cell_value)
    elif agg_type == "mobile_oil_column":
        cell_value = np.maximum(soil_3d - DEFAULT_SOWCR, 0.0) * ntg_3d * poro_3d * thickness
        weight = np.ones_like(cell_value)
    elif agg_type == "mobile_gas_column":
        cell_value = np.maximum(sgas_3d - DEFAULT_SGCR, 0.0) * ntg_3d * poro_3d * thickness
        weight = np.ones_like(cell_value)
    elif agg_type == "mobile_hydrocarbon_column":
        oil_part = np.maximum(soil_3d - DEFAULT_SOWCR, 0.0)
        gas_part = np.maximum(sgas_3d - DEFAULT_SGCR, 0.0)
        cell_value = (oil_part + gas_part) * ntg_3d * poro_3d * thickness
        weight = np.ones_like(cell_value)
    elif agg_type == "volume_weighted_sum":
        cell_value = soil_3d * ntg_3d * poro_3d * thickness
        weight = np.ones_like(cell_value)
    elif agg_type == "sum":
        cell_value = soil_3d
        weight = np.ones_like(cell_value)
    else:
        print(f"错误: 未知的聚合类型 '{agg_type}'")
        sys.exit(1)

    cell_value[actnum_3d == 0] = np.nan
    weight[actnum_3d == 0] = np.nan

    print(f"有效单元数: {np.sum(~np.isnan(cell_value))}, 值范围: [{np.nanmin(cell_value):.6f}, {np.nanmax(cell_value):.6f}]")

    X, Y, vertex_values = build_projection_grid_intersection(
        cell_polygons, cell_value, weight, actnum_3d, resolution=DEFAULT_RESOLUTION
    )

    title = f"{agg_type.upper()} - Time Step {time_step}"
    plot_contour_from_vertex_grid(X, Y, vertex_values, title, OUTPUT_FILE,
                                  contour_levels=DEFAULT_CONTOUR_LEVELS, swap_xy=SWAP_XY)

if __name__ == "__main__":
    main()