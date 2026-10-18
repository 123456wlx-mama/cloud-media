#ifndef MYTABLEWIDGETITEM_H
#define MYTABLEWIDGETITEM_H

#include <QTableWidgetItem>
#include "common.h"

class MainDialog;
class MyTableWidgetItem : public QTableWidgetItem
{
    //Q_OBJECT
public:
    MyTableWidgetItem();
public slots:
    void slot_setInfo( FileInfo & info );
private:
    FileInfo m_info;
    friend class MainDialog;//友元类
};

#endif // MYTABLEWIDGETITEM_H
