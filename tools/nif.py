"""Static visible triangle geometry from Skyrim SE NIFs (20.2.0.7 / 12 / 100).

This is a silhouette reader, not a renderer: collision, hidden objects, ordered
LOD branches, editor markers, decals and blended cards are excluded.
"""
import re
import struct

import numpy as np


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, size):
        if size < 0 or self.at + size > len(self.data):
            raise ValueError('truncated NIF')
        value = self.data[self.at:self.at + size]
        self.at += size
        return value

    def get(self, fmt):
        values = struct.unpack('<' + fmt, self.take(struct.calcsize('<' + fmt)))
        return values[0] if len(values) == 1 else values

    def string(self, small=False):
        return self.take(self.get('B' if small else 'I')).rstrip(b'\0').decode('utf-8', 'replace')

    def refs(self):
        return [v[0] for v in struct.iter_unpack('<i', self.take(self.get('I') * 4))]


def _object(reader):
    name = reader.get('i')
    reader.refs()  # extra data
    reader.get('i')  # controller
    return name


def _av(reader):
    name = _object(reader)
    flags = reader.get('I')
    translation = reader.get('3f')
    rotation = np.array(reader.get('9f'), dtype=np.float64).reshape(3, 3)
    scale = reader.get('f')
    reader.get('i')  # collision object, deliberately not traversed
    transform = np.eye(4)
    transform[:3, :3] = rotation * scale
    transform[:3, 3] = translation
    if not np.isfinite(transform).all():
        raise ValueError('non-finite NIF transform')
    return name, flags, transform


def _packed(reader):
    reader.take(16)  # bounding sphere
    _, shader, alpha = reader.get('3i')
    descriptor = reader.get('Q')
    triangles, vertices, size = reader.get('HHI')
    stride, attributes = (descriptor & 15) * 4, descriptor >> 44
    if not attributes & 1 or not vertices:
        if size:
            raise ValueError('NIF has no static vertex positions')
        return np.empty((0, 3)), np.empty((0, 3), dtype=np.uint16), shader, alpha
    # SSE stores float4 positions even without VF_FULLPREC. Attribute offsets
    # delimit that slot; compact descriptors use half4 instead.
    offsets = [(descriptor >> shift & 15) * 4 for shift in range(8, 40, 4)]
    position_size = min([offset for offset in offsets if offset] or [stride])
    full = bool(attributes & 0x400) or position_size >= 16
    if stride < (12 if full else 6) or size != vertices * stride + triangles * 6:
        raise ValueError('invalid NIF vertex descriptor/data size')
    raw = reader.take(vertices * stride)
    positions = np.ndarray((vertices, 3), dtype='<f4' if full else '<f2', buffer=raw,
                           strides=(stride, 4 if full else 2)).astype(np.float64)
    indices = np.frombuffer(reader.take(triangles * 6), dtype='<u2').reshape(-1, 3)
    return positions, indices, shader, alpha


def _legacy(reader):
    reader.get('I')  # group ID
    count = reader.get('H')
    reader.take(2)  # keep/compress flags
    if not reader.get('B'):
        raise ValueError('NiTriShapeData has no vertices')
    positions = np.frombuffer(reader.take(count * 12), dtype='<f4').reshape(-1, 3).astype(np.float64)
    flags = reader.get('H')
    reader.get('I')  # Bethesda material CRC
    if reader.get('B'):
        reader.take(count * 12 * (3 if flags & 0x1000 else 1))
    reader.take(16)  # bounding sphere
    if reader.get('B'):
        reader.take(count * 16)
    reader.take((flags & 63) * count * 8)  # UV sets
    reader.get('H')  # consistency
    reader.get('i')  # additional geometry data
    triangles, points = reader.get('HI')
    if not reader.get('B') or points != triangles * 3:
        raise ValueError('NiTriShapeData has no triangles')
    indices = np.frombuffer(reader.take(triangles * 6), dtype='<u2').reshape(-1, 3)
    return positions, indices


def triangles(data):
    """Return an (N,3,2) array of model-space triangles, with root/node/shape transforms applied.

    Empty visible geometry is a successful read (never replace alpha/hidden
    geometry by its OBND). Unsupported visible scene blocks fail explicitly.
    """
    reader = Reader(data)
    header = b'Gamebryo File Format, Version 20.2.0.7\n'
    if reader.take(len(header)) != header:
        raise ValueError('unsupported NIF header')
    version, endian, user, count, bs = reader.get('IBIII')
    if (version, endian, user, bs) != (0x14020007, 1, 12, 100):
        raise ValueError('unsupported NIF version')
    for _ in range(3):
        reader.string(small=True)
    types = [reader.string() for _ in range(reader.get('H'))]
    type_ids = [reader.get('H') for _ in range(count)]
    sizes = [reader.get('I') for _ in range(count)]
    strings_count = reader.get('I')
    reader.get('I')  # maximum string length
    strings = [reader.string() for _ in range(strings_count)]
    reader.take(reader.get('I') * 4)  # groups
    blocks = []
    for type_id, size in zip(type_ids, sizes):
        if type_id >= len(types):
            raise ValueError('invalid NIF block type')
        blocks.append((types[type_id], reader.take(size)))
    roots = reader.refs()
    output = []
    node_types = {'NiNode', 'BSFadeNode', 'BSLeafAnimNode', 'BSMultiBoundNode', 'NiSwitchNode', 'NiLODNode'}
    shape_types = {'BSTriShape', 'BSSubIndexTriShape', 'NiTriShape'}

    def block(index):
        if not 0 <= index < len(blocks):
            raise ValueError('invalid NIF block reference')
        kind, raw = blocks[index]
        return kind, Reader(raw)

    def alpha_card(index):
        if index == -1:
            return False
        kind, r = block(index)
        if kind != 'NiAlphaProperty':
            raise ValueError('invalid NIF alpha property')
        _object(r)
        return bool(r.get('H') & 1)  # blending; alpha-tested thatch/shingles are solid roofs

    def visit(index, parent, ancestors):
        if index == -1:
            return
        if index in ancestors or len(ancestors) > 128:
            raise ValueError('cyclic/deep NIF scene')
        kind, r = block(index)
        if kind == 'BSOrderedNode' or kind.startswith(('bhk', 'NiCollision', 'BSValueNode')):
            return
        if kind not in node_types | shape_types:
            raise ValueError(f'unsupported NIF scene block: {kind}')
        name, flags, local = _av(r)
        if name != -1 and not 0 <= name < len(strings):
            raise ValueError('invalid NIF name')
        label = strings[name] if name != -1 else ''
        # Bit 0 is APP_CULLED; other flags include update/render hints, not visibility.
        if flags & 1 or re.search(r'editor.?marker|^marker', label, re.I):
            return
        transform = parent @ local
        if kind in node_types:
            children = r.refs()
            if kind == 'NiSwitchNode':
                r.refs()  # NiNode effects
                r.get('H')
                active = r.get('I')
                children = children[active:active + 1]
            if kind == 'NiLODNode':
                children = children[:1]  # highest detail only
            for child in children:
                visit(child, transform, ancestors | {index})
            return
        if kind in {'BSTriShape', 'BSSubIndexTriShape'}:
            positions, indices, shader, alpha = _packed(r)
        else:
            geometry, skin = r.get('2i')
            materials = r.get('I')
            r.take(materials * 8)
            r.get('i')  # active material
            r.get('B')  # needs update
            shader, alpha = r.get('2i')
            geometry_type, geometry_reader = block(geometry)
            if geometry_type != 'NiTriShapeData':
                raise ValueError('unsupported NiTriShape data')
            positions, indices = _legacy(geometry_reader)
        if alpha_card(alpha):
            return
        if shader != -1:
            shader_type, sr = block(shader)
            if shader_type == 'BSEffectShaderProperty':
                return  # effects/glow cards, not opaque surfaces
            if shader_type == 'BSLightingShaderProperty':
                sr.get('I')  # shader type precedes NiObjectNET in BS100
                _object(sr)
                flags1, flags2 = sr.get('2I')
                # Decal/dynamic-decal surfaces are alpha cards laid over the mesh.
                if flags1 & 0x0c000000:
                    return
                sr.take(16)  # UV offset/scale
                textures = sr.get('i')
                if textures != -1:
                    texture_type, tr = block(textures)
                    if texture_type != 'BSShaderTextureSet':
                        raise ValueError('invalid NIF texture set')
                    if tr.get('I'):
                        diffuse = tr.string().replace('\\', '/').casefold()
                        # Composite city STATs include streets/courtyards and vegetation.
                        # The material identifies these even when the shape is named Object123.
                        if re.search(r'ground|street|floor|terrain|landscape/|dirt|mud|moss|ivy|leaves|decal|shadow', diffuse):
                            return
                sr.take(12)  # emissive colour
                sr.get('f')  # emissive multiplier
                sr.get('I')  # clamp mode
                if sr.get('f') < 0.999:  # material alpha
                    return
        if not np.isfinite(positions).all() or indices.size and indices.max() >= len(positions):
            raise ValueError('invalid NIF geometry')
        if len(indices):
            world = positions @ transform[:3, :3].T + transform[:3, 3]
            projected = world[indices, :2]
            a, b = projected[:, 1] - projected[:, 0], projected[:, 2] - projected[:, 0]
            output.append(projected[np.abs(a[:, 0] * b[:, 1] - a[:, 1] * b[:, 0]) > 1e-8])

    for root in roots:
        visit(root, np.eye(4), set())
    return np.concatenate(output) if output else np.empty((0, 3, 2))
