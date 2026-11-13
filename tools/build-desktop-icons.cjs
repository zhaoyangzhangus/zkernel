// WhiteSur icon theme by vinceliuice, GPL-3.0 (COPYING beside the source SVGs).
// 来源：https://github.com/vinceliuice/WhiteSur-icon-theme/tree/2026-09-10/src/apps/scalable
// 重新生成已提交的 PNG 图集；操作系统构建无需 SVG 渲染器。
const sharp = require('sharp');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const icons = ['terminal', 'accessories-text-editor', 'network-workgroup',
               'utilities-system-monitor', 'google-chrome'];
(async () => {
    const layers = await Promise.all(icons.map(async (name, i) => ({
        input: await sharp(path.join(root, 'assets/desktop/whitesur', name + '.svg'),
                           {density: 384}).resize(256, 256).png().toBuffer(),
        left: i * 256, top: 0,
    })));
    await sharp({create: {width: 1280, height: 256, channels: 4,
                         background: {r: 0, g: 0, b: 0, alpha: 0}}})
        .composite(layers).png().toFile(path.join(root, 'assets/desktop/whitesur-icons.png'));
    console.log('WhiteSur desktop atlas: five 256x256 icons, transparent RGBA');
})().catch(error => { console.error(error); process.exitCode = 1; });
