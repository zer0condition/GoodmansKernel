#pragma once
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QListView>
#include <QKeyEvent>
#include <functional>

struct CommandAction {
    QString label;
    QString hint;         // shortcut or context
    std::function<void()> run;
};

class CommandPalette : public QDialog {
    Q_OBJECT
public:
    CommandPalette(QWidget* parent, const QVector<CommandAction>& actions);
protected:
    void keyPressEvent(QKeyEvent* e) override;
private:
    QLineEdit*             search;
    QListView*             list;
    QStandardItemModel*    model;
    QSortFilterProxyModel* proxy;
    QVector<CommandAction> all_actions;
    void runSelected();
};
