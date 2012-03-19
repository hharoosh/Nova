//============================================================================
// Name        : nodePopup.h
// Copyright   : DataSoft Corporation 2011-2012
//	Nova is free software: you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation, either version 3 of the License, or
//   (at your option) any later version.
//
//   Nova is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with Nova.  If not, see <http://www.gnu.org/licenses/>.
// Description : Popup for creating and editing nodes
//============================================================================
#ifndef NODEPOPUP_H
#define NODEPOPUP_H

#include "ui_nodePopup.h"
#include "NovaGuiTypes.h"

#include <QtGui/QSpinBox>
#include <math.h>
#include <string.h>

enum macType{macPrefix = 0, macSuffix = 1};

class QRegExpValidator;

class HexMACSpinBox : public QSpinBox
{
	Q_OBJECT

public:
	HexMACSpinBox(QWidget * parent = 0, std::string MACAddr = "", macType which = macSuffix) : QSpinBox(parent)
	{
		validator = new QRegExpValidator(QRegExp("([0-9A-Fa-f][0-9A-Fa-f][:]){0,2}[0-9A-Fa-f][0-9A-Fa-f]"
				, Qt::CaseInsensitive, QRegExp::RegExp), this);
		setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
		setWrapping(true);
		setRange(0, (::pow(2,24)-1));

		if(MACAddr.size() < 17)
		{
			setValue(0);
		}

		else if(which == macSuffix)
		{
			QString suffixStr = QString(MACAddr.substr(9, MACAddr.size()).c_str()).toLower();
			suffixStr = suffixStr.remove(':');
			setValue(suffixStr.toInt(NULL, 16));
		}
		else
		{
			QString prefixStr = QString(MACAddr.substr(0, 8).c_str()).toLower();
			prefixStr = prefixStr.remove(':');
			setValue(prefixStr.toInt(NULL, 16));
		}
	}

protected:
	int valueFromText(const QString &text) const
	{
		int val = 0;
		QString temp = QString(text);
		temp = temp.remove(':');
		temp = temp.right(6);
		val = temp.toInt(NULL, 16);
		return val;
	}

	QString textFromValue(int value) const
	{
		QString ret = QString::number(value, 16).toLower();
		while(ret.toStdString().size() < 6)
		{
			ret = ret.prepend('0');
		}
		ret = ret.insert(4, ':');
		ret = ret.insert(2, ':');
		return ret;
	}

	QValidator::State validate(QString &text, int &pos) const
	{
		if(this->lineEdit()->text().size() < 8)
			return QValidator::Intermediate;
		return validator->validate(text, pos);
	}

private:

    QRegExpValidator *validator;
};


class nodePopup : public QMainWindow
{
    Q_OBJECT

public:

    nodePopup(QWidget *parent = 0, node *n  = NULL);
    ~nodePopup();

    HexMACSpinBox * ethernetEdit;
    HexMACSpinBox * prefixEthEdit;

    //Saves the current configuration
    void saveNode();
    //Loads the last saved configuration
    void loadNode();
    //Copies the data from parent novaconfig and adjusts the pointers
    void pullData();
    //Copies the data to parent novaconfig and adjusts the pointers
    void pushData();

    //Checks for IP or MAC conflicts
    int validateNodeSettings();

private Q_SLOTS:

	//General Button slots
	void on_okButton_clicked();
	void on_cancelButton_clicked();
	void on_restoreButton_clicked();
	void on_applyButton_clicked();
	void on_generateButton_clicked();

private:
    Ui::nodePopupClass ui;
};

#endif // NODEPOPUP_H
