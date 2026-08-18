#include "command_palette.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QStyledItemDelegate>

// Fuzzy sort proxy: match if all chars in query appear in order in target.
// Scoring: fewer gaps + earlier match wins. Used to power the "type anywhere" flow.
class FuzzyProxy : public QSortFilterProxyModel {
public:
    explicit FuzzyProxy(QObject* p = nullptr) : QSortFilterProxyModel(p) { setDynamicSortFilter(true); }
    void setQuery(const QString& q) { query = q.toLower(); invalidateFilter(); invalidate(); }
protected:
    bool filterAcceptsRow(int row, const QModelIndex&) const override {
        if (query.isEmpty()) return true;
        QString s = sourceModel()->data(sourceModel()->index(row, 0)).toString().toLower();
        int qi = 0;
        for (int i = 0; i < s.size() && qi < query.size(); i++) if (s[i] == query[qi]) qi++;
        return qi == query.size();
    }
    bool lessThan(const QModelIndex& l, const QModelIndex& r) const override {
        if (query.isEmpty()) return l.row() < r.row();
        auto score = [&](const QString& s)->int {
            int qi = 0, first = -1, gaps = 0, prev = -100;
            QString low = s.toLower();
            for (int i = 0; i < low.size() && qi < query.size(); i++) {
                if (low[i] == query[qi]) {
                    if (first == -1) first = i;
                    if (prev != i - 1) gaps++;
                    prev = i; qi++;
                }
            }
            return first * 4 + gaps * 8 + s.size();
        };
        int sl = score(sourceModel()->data(l).toString());
        int sr = score(sourceModel()->data(r).toString());
        return sl < sr;
    }
private:
    QString query;
};

class PaletteDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return {0, 34}; }
    void paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& idx) const override {
        p->save();
        if (o.state & QStyle::State_Selected) p->fillRect(o.rect, QColor(0x2a, 0x21, 0x17));
        auto full = idx.data().toString();
        auto hint = idx.data(Qt::UserRole + 1).toString();
        p->setPen((o.state & QStyle::State_Selected) ? QColor(0xf0, 0xa3, 0x40) : QColor(0xd4, 0xd7, 0xdd));
        QFont f = p->font(); f.setPointSizeF(11.5); p->setFont(f);
        p->drawText(QRect(o.rect.left() + 16, o.rect.top(), o.rect.width() - 200, o.rect.height()),
                    Qt::AlignVCenter | Qt::AlignLeft, full);
        if (!hint.isEmpty()) {
            p->setPen(QColor(0x6a, 0x72, 0x80));
            QFont hf = f; hf.setPointSizeF(10.5); hf.setFamily("JetBrainsMono NF"); p->setFont(hf);
            p->drawText(QRect(o.rect.right() - 180, o.rect.top(), 164, o.rect.height()),
                        Qt::AlignVCenter | Qt::AlignRight, hint);
        }
        p->restore();
    }
};

CommandPalette::CommandPalette(QWidget* parent, const QVector<CommandAction>& actions)
    : QDialog(parent), all_actions(actions)
{
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setStyleSheet(
        "QDialog { background: #14171c; border: 1px solid #363c47; border-radius: 10px; }"
        "QLineEdit { background: transparent; border: none; border-bottom: 1px solid #262a33;"
        "  padding: 14px 18px; font-size: 15px; color: #f0f2f7; }"
        "QListView { background: transparent; border: none; outline: 0; }"
    );

    resize(640, 420);

    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);

    search = new QLineEdit;
    search->setPlaceholderText("Type a command…");
    l->addWidget(search);

    model = new QStandardItemModel(this);
    for (const auto& a : all_actions) {
        auto* it = new QStandardItem(a.label);
        it->setData(a.hint, Qt::UserRole + 1);
        model->appendRow(it);
    }
    proxy = new FuzzyProxy(this);
    proxy->setSourceModel(model);

    list = new QListView;
    list->setModel(proxy);
    list->setItemDelegate(new PaletteDelegate(this));
    list->setUniformItemSizes(true);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    l->addWidget(list, 1);

    if (proxy->rowCount() > 0)
        list->setCurrentIndex(proxy->index(0, 0));

    connect(search, &QLineEdit::textChanged, this, [this](const QString& t) {
        static_cast<FuzzyProxy*>(proxy)->setQuery(t);
        proxy->sort(0);
        if (proxy->rowCount() > 0) list->setCurrentIndex(proxy->index(0, 0));
    });
    connect(list, &QListView::activated, this, [this](const QModelIndex&) { runSelected(); });

    search->setFocus();
}

void CommandPalette::runSelected()
{
    auto idx = list->currentIndex();
    if (!idx.isValid()) return;
    auto src = proxy->mapToSource(idx);
    int i = src.row();
    if (i < 0 || i >= all_actions.size()) return;
    auto fn = all_actions[i].run;
    accept();
    if (fn) fn();
}

void CommandPalette::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) { reject(); return; }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) { runSelected(); return; }
    if (e->key() == Qt::Key_Up) {
        auto cur = list->currentIndex().row();
        if (cur > 0) list->setCurrentIndex(proxy->index(cur - 1, 0));
        return;
    }
    if (e->key() == Qt::Key_Down) {
        auto cur = list->currentIndex().row();
        if (cur < proxy->rowCount() - 1) list->setCurrentIndex(proxy->index(cur + 1, 0));
        return;
    }
    QDialog::keyPressEvent(e);
}
