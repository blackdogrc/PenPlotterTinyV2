static const char SVG_CONVERTER_JS[] PROGMEM = R"SVGJS(
class SvgConverter {
  static NS = 'http://www.w3.org/2000/svg';

  static numberWithUnit(value, fallback) {
    if (!value) return fallback;

    const m = String(value).trim().match(
      /^([+-]?(?:\d+\.?\d*|\.\d+))(mm|cm|in|px|pt|pc)?$/i
    );
    if (!m) return fallback;

    const n = Number(m[1]);
    switch ((m[2] || '').toLowerCase()) {
      case 'cm': return n * 10;
      case 'in': return n * 25.4;
      case 'px': return n * 25.4 / 96;
      case 'pt': return n * 25.4 / 72;
      case 'pc': return n * 25.4 / 6;
      default:   return n; // mm or unitless
    }
  }

  static clipSegment(a, b, width, height) {
    const dx = b.x - a.x;
    const dy = b.y - a.y;
    const p = [-dx, dx, -dy, dy];
    const q = [a.x, width - a.x, a.y, height - a.y];
    let t0 = 0;
    let t1 = 1;

    for (let i = 0; i < 4; i++) {
      if (p[i] === 0) {
        if (q[i] < 0) return null;
        continue;
      }

      const r = q[i] / p[i];

      if (p[i] < 0) {
        if (r > t1) return null;
        if (r > t0) t0 = r;
      } else {
        if (r < t0) return null;
        if (r < t1) t1 = r;
      }
    }

    return [
      { x: a.x + t0 * dx, y: a.y + t0 * dy },
      { x: a.x + t1 * dx, y: a.y + t1 * dy }
    ];
  }

  // Iterative Ramer–Douglas–Peucker simplification.
  // Points and tolerance are in plotted millimeters.
  static simplifyPolyline(points, tolerance = 0.15) {
    if (points.length < 3 || tolerance <= 0) return points;

    const toleranceSq = tolerance * tolerance;
    const keep = new Uint8Array(points.length);
    keep[0] = 1;
    keep[points.length - 1] = 1;

    const stack = [[0, points.length - 1]];

    while (stack.length) {
      const [first, last] = stack.pop();
      const a = points[first];
      const b = points[last];
      const dx = b.x - a.x;
      const dy = b.y - a.y;
      const lengthSq = dx * dx + dy * dy;

      let maxDistanceSq = 0;
      let farthest = -1;

      for (let i = first + 1; i < last; i++) {
        const p = points[i];

        let t = lengthSq === 0
          ? 0
          : ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSq;

        t = Math.max(0, Math.min(1, t));

        const nearestX = a.x + t * dx;
        const nearestY = a.y + t * dy;
        const distanceSq =
          (p.x - nearestX) * (p.x - nearestX) +
          (p.y - nearestY) * (p.y - nearestY);

        if (distanceSq > maxDistanceSq) {
          maxDistanceSq = distanceSq;
          farthest = i;
        }
      }

      if (farthest !== -1 && maxDistanceSq > toleranceSq) {
        keep[farthest] = 1;
        stack.push([first, farthest], [farthest, last]);
      }
    }

    return points.filter((_, i) => keep[i]);
  }

  // Remove redundant points along straight horizontal or vertical runs.
  static simplifyAxisAligned(points) {
    if (points.length < 3) return points;

    const result = [points[0]];
    const rounded = v => Math.round(v * 1000) / 1000;

    for (let i = 1; i < points.length; i++) {
      const next = points[i];

      if (result.length >= 2) {
        const a = result[result.length - 2];
        const b = result[result.length - 1];

        const ax = rounded(a.x);
        const ay = rounded(a.y);
        const bx = rounded(b.x);
        const by = rounded(b.y);
        const cx = rounded(next.x);
        const cy = rounded(next.y);

        const horizontal =
          ay === by && by === cy &&
          (bx - ax) * (cx - bx) >= 0;

        const vertical =
          ax === bx && bx === cx &&
          (by - ay) * (cy - by) >= 0;

        if (horizontal || vertical) {
          result[result.length - 1] = next;
          continue;
        }
      }

      result.push(next);
    }

    return result;
  }

  // Join adjacent segment blocks when their endpoints meet.
  static joinContinuousSegments(lines) {
    const result = [];
    let previousEnd = null;

    const parseXY = (line, command) => {
      const match = line.match(
        new RegExp(
          '^' + command +
          '\\s+X([-+]?\\d+(?:\\.\\d+)?)' +
          '\\s+Y([-+]?\\d+(?:\\.\\d+)?)\\b'
        )
      );

      return match
        ? { x: Number(match[1]), y: Number(match[2]) }
        : null;
    };

    const samePoint = (a, b) =>
      Math.abs(a.x - b.x) <= 0.001 &&
      Math.abs(a.y - b.y) <= 0.001;

    for (let i = 0; i < lines.length;) {
      if (
        lines[i] === 'M5' &&
        i + 3 < lines.length &&
        lines[i + 2] === 'M3'
      ) {
        const start = parseXY(lines[i + 1], 'G0');
        const end = parseXY(lines[i + 3], 'G1');

        if (start && end) {
          if (previousEnd && samePoint(previousEnd, start)) {
            result.push(lines[i + 3]);
          } else {
            result.push(
              lines[i],
              lines[i + 1],
              lines[i + 2],
              lines[i + 3]
            );
          }

          previousEnd = end;
          i += 4;
          continue;
        }
      }

      result.push(lines[i]);
      previousEnd = null;
      i++;
    }

    return result;
  }

  static async convert(
    file,
    width,
    height,
    mode,
    drawingFeed,
    simplifyTolerance = 0.15,
    ignoreViewBoxFrame = false
  ) {
    let maxX = 0;
    let maxY = 0;
    if (!Number.isFinite(simplifyTolerance) || simplifyTolerance < 0) {
      throw new Error('Simplification tolerance must be zero or greater.');
    }

    let sampledPointCount = 0;
    let simplifiedPointCount = 0;

    const source = await file.text();
    const doc = new DOMParser().parseFromString(source, 'image/svg+xml');

    if (doc.querySelector('parsererror')) {
      throw new Error('Could not parse this SVG file.');
    }

    const root = doc.documentElement;
    if (!root || root.localName !== 'svg') {
      throw new Error('The file does not contain an SVG document.');
    }

    const viewBoxText = root.getAttribute('viewBox');
    let vb;

    if (viewBoxText) {
      const values = viewBoxText.trim().split(/[\s,]+/).map(Number);

      if (
        values.length !== 4 ||
        values.some(v => !Number.isFinite(v)) ||
        values[2] <= 0 ||
        values[3] <= 0
      ) {
        throw new Error('The SVG has an invalid viewBox.');
      }

      vb = { x: values[0], y: values[1], w: values[2], h: values[3] };
    } else {
      const w = SvgConverter.numberWithUnit(root.getAttribute('width'), 0);
      const h = SvgConverter.numberWithUnit(root.getAttribute('height'), 0);

      if (w <= 0 || h <= 0) {
        throw new Error('SVG needs a viewBox or explicit width and height.');
      }

      vb = { x: 0, y: 0, w, h };
    }

    const physicalW = SvgConverter.numberWithUnit(
      root.getAttribute('width'),
      vb.w
    );
    const physicalH = SvgConverter.numberWithUnit(
      root.getAttribute('height'),
      vb.h
    );

    let scale = 1;
    if (mode === 'fit') {
      scale = Math.min(width / physicalW, height / physicalH);
    }

    const drawnW = physicalW * scale;
    const drawnH = physicalH * scale;
    const offsetX = (width - drawnW) / 2;
    const offsetY = (height - drawnH) / 2;

    // Attach the SVG temporarily so the browser can evaluate geometry
    // and transforms.
    const host = document.createElementNS(SvgConverter.NS, 'svg');
    host.setAttribute('viewBox', `${vb.x} ${vb.y} ${vb.w} ${vb.h}`);
    host.setAttribute('width', '1000');
    host.setAttribute('height', '1000');
    host.style.cssText =
      'position:fixed;left:-2000px;top:-2000px;width:1000px;height:1000px';

    document.body.appendChild(host);

    const imported = document.importNode(root, true);
    host.appendChild(imported);

    const rootMatrix = imported.getScreenCTM();

    if (!rootMatrix) {
      host.remove();
      throw new Error('Could not read SVG transforms.');
    }

    const inverse = rootMatrix.inverse();

    const allDrawable = [
      ...imported.querySelectorAll(
        'path,line,polyline,polygon,rect,circle,ellipse'
      )
    ];

    const closeTo = (a, b, tolerance = 0.01) =>
      Math.abs(a - b) <= tolerance;

    // Optionally skip a <rect> exactly matching the SVG viewBox.
    // This does not detect frames drawn as paths or other shapes.
    const drawable = allDrawable.filter(shape => {
      if (!ignoreViewBoxFrame || shape.localName !== 'rect') return true;

      const x = Number(shape.getAttribute('x') || 0);
      const y = Number(shape.getAttribute('y') || 0);
      const w = Number(shape.getAttribute('width'));
      const h = Number(shape.getAttribute('height'));

      const matchesViewBox =
        Number.isFinite(w) &&
        Number.isFinite(h) &&
        closeTo(x, vb.x) &&
        closeTo(y, vb.y) &&
        closeTo(w, vb.w) &&
        closeTo(h, vb.h);

      return !matchesViewBox;
    });

    if (!drawable.length) {
      host.remove();
      throw new Error('No supported drawing shapes were found in the SVG.');
    }

    const paths = [];
    let unsupported = false;

    try {
      for (const shape of drawable) {
        if (
          typeof shape.getTotalLength !== 'function' ||
          typeof shape.getPointAtLength !== 'function'
        ) {
          unsupported = true;
          continue;
        }

        let length;
        try {
          length = shape.getTotalLength();
        } catch (_) {
          unsupported = true;
          continue;
        }

        if (!Number.isFinite(length) || length <= 0) continue;

        // Limit sampling to at most 3000 steps for each shape.
        const steps = Math.max(1, Math.min(3000, Math.ceil(length / 2)));
        const points = [];
        const shapeMatrix = shape.getScreenCTM();

        if (!shapeMatrix) {
          unsupported = true;
          continue;
        }

        for (let i = 0; i <= steps; i++) {
          const p = shape.getPointAtLength(length * i / steps);
          const screenPoint =
            new DOMPoint(p.x, p.y).matrixTransform(shapeMatrix);
          const q = screenPoint.matrixTransform(inverse);

          const x =
            (q.x - vb.x) * physicalW / vb.w * scale + offsetX;
          const y =
            (q.y - vb.y) * physicalH / vb.h * scale + offsetY;

          points.push({ x, y });
        }

        sampledPointCount += points.length;

        const cleaned = SvgConverter.simplifyAxisAligned(points);
        const simplified = SvgConverter.simplifyPolyline(
          cleaned,
          simplifyTolerance
        );

        simplifiedPointCount += simplified.length;
        paths.push(simplified);
      }
    } finally {
      host.remove();
    }

    if (!paths.length) {
      throw new Error('No usable paths were found in the SVG.');
    }

    const out = ['G21', 'G90', 'M5'];
    let drawnSegments = 0;
    let feedWritten = false;

    for (const points of paths) {
      let penDown = false;
      let lastEnd = null;

      for (let i = 1; i < points.length; i++) {
        const clipped = SvgConverter.clipSegment(
          points[i - 1],
          points[i],
          width,
          height
        );

        if (!clipped) {
          if (penDown) out.push('M5');
          penDown = false;
          lastEnd = null;
          continue;
        }

        const [a, b] = clipped;
        if (Math.hypot(b.x - a.x, b.y - a.y) < 0.001) continue;

        maxX = Math.max(maxX, a.x, b.x);
        maxY = Math.max(maxY, height - a.y, height - b.y);
        if (Math.hypot(b.x - a.x, b.y - a.y) < 0.001) continue;

        const samePoint = lastEnd &&
          Math.hypot(lastEnd.x - a.x, lastEnd.y - a.y) < 0.01;

        if (!penDown || !samePoint) {
          if (penDown) out.push('M5');

          out.push(
            `G0 X${a.x.toFixed(3)} Y${(height - a.y).toFixed(3)}`,
            'M3'
          );
          penDown = true;
        }

        const bx = b.x.toFixed(3);
        const by = (height - b.y).toFixed(3);

        if (!feedWritten) {
          out.push(`G1 X${bx} Y${by} F${Math.round(drawingFeed)}`);
          feedWritten = true;
        } else {
          out.push(`G1 X${bx} Y${by}`);
        }

        lastEnd = b;
        drawnSegments++;
      }

      if (penDown) out.push('M5');
    }

    out.push('M30');

    if (!drawnSegments) {
      throw new Error('Nothing from this SVG falls inside the plot area.');
    }

    const optimizedOut = SvgConverter.joinContinuousSegments(out);

    return {
      gcode: optimizedOut.join('\n'),
      warning: unsupported
        ? 'Some SVG shapes were skipped because they could not be converted.'
        : '',
      stats: {
        inputBytes: file.size,
        shapeCount: drawable.length,
        sampledPointCount,
        simplifiedPointCount,
        removedPointCount: sampledPointCount - simplifiedPointCount,
        simplifyTolerance,
        gcodeLineCount: optimizedOut.length,
        maxX: Number(maxX.toFixed(3)),
        maxY: Number(maxY.toFixed(3))
      }
    };
  }
}
)SVGJS";
