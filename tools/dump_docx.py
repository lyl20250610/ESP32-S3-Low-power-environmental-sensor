# -*- coding: utf-8 -*-
"""把项目书 DOCX 完整导出为纯文本，便于阅读与后续分步实施。"""
import sys
from docx import Document
from docx.table import Table
from docx.text.paragraph import Paragraph


def iter_block_items(parent):
    from docx.oxml.ns import qn
    body = parent.element.body
    for child in body.iterchildren():
        if child.tag == qn('w:p'):
            yield Paragraph(child, parent)
        elif child.tag == qn('w:tbl'):
            yield Table(child, parent)


def main(src, dst):
    doc = Document(src)
    out = []
    ti = 0
    for block in iter_block_items(doc):
        if isinstance(block, Paragraph):
            txt = block.text.rstrip()
            style = block.style.name if block.style is not None else ''
            if not txt:
                out.append('')
                continue
            if style.startswith('Heading') or style.startswith('标题'):
                out.append('')
                out.append(f'### [{style}] {txt}')
            else:
                out.append(txt)
        else:
            ti += 1
            out.append('')
            out.append(f'--- 表格 {ti} ---')
            for row in block.rows:
                cells = [c.text.replace('\n', ' / ').strip() for c in row.cells]
                out.append(' | '.join(cells))
            out.append('--- 表格结束 ---')
            out.append('')
    text = '\n'.join(out)
    with open(dst, 'w', encoding='utf-8') as f:
        f.write(text)
    print(f'paragraphs_blocks_written={len(out)} chars={len(text)}')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
