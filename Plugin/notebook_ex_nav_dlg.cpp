//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
//
// copyright            : (C) 2008 by Eran Ifrah
// file name            : notebooknavdialog.cpp
//
// -------------------------------------------------------------------------
// A
//              _____           _      _     _ _
//             /  __ \         | |    | |   (_) |
//             | /  \/ ___   __| | ___| |    _| |_ ___
//             | |    / _ \ / _  |/ _ \ |   | | __/ _ )
//             | \__/\ (_) | (_| |  __/ |___| | ||  __/
//              \____/\___/ \__,_|\___\_____/_|\__\___|
//
//                                                  F i l e
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

#include "wx/settings.h"
#include "notebook_ex_nav_dlg.h"
#include <wx/listctrl.h>
#include <wx/listbox.h>
#include <wx/image.h>
#include "wx/sizer.h"
#include "notebook_ex.h"

BEGIN_EVENT_TABLE(NotebookNavDialog, wxDialog)
	EVT_NAVIGATION_KEY(NotebookNavDialog::OnNavigationKey)
END_EVENT_TABLE()

NotebookNavDialog::NotebookNavDialog(wxWindow* parent)
	: m_listBox(NULL)
	, m_selectedItem(static_cast<size_t>(-1))
	, m_selTab(NULL)
{
	Create(parent);
	GetSizer()->Fit(this);
	GetSizer()->SetSizeHints(this);
	GetSizer()->Layout();
	Centre();
}

NotebookNavDialog::NotebookNavDialog()
	: wxDialog()
	, m_listBox(NULL)
	, m_selectedItem(static_cast<size_t>(-1))
{
}

NotebookNavDialog::~NotebookNavDialog()
{
}

void NotebookNavDialog::Create(wxWindow* parent)
{
	if (  !wxDialog::Create(parent, wxID_ANY, wxEmptyString) )
		return;
		
	wxBoxSizer *sz = new wxBoxSizer( wxVERTICAL );
	SetSizer( sz );

	long flags = wxLB_SINGLE | wxNO_BORDER | wxWANTS_CHARS;
	m_listBox = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(400, 400), 0, NULL, flags);

	sz->Add( m_listBox, 1, wxEXPAND );
	SetSizer( sz );

	// Connect events to the list box
	m_listBox->Connect(wxID_ANY, wxEVT_KEY_UP,   wxKeyEventHandler(NotebookNavDialog::OnKeyUp),   NULL, this);
	m_listBox->Connect(wxID_ANY, wxEVT_KEY_DOWN, wxKeyEventHandler(NotebookNavDialog::OnKeyDown), NULL, this);
	m_listBox->Connect(wxID_ANY, wxEVT_COMMAND_LISTBOX_DOUBLECLICKED, wxCommandEventHandler(NotebookNavDialog::OnItemSelected), NULL, this);
	
	SetBackgroundColour( wxSystemSettings::GetColour(wxSYS_COLOUR_3DFACE) );
	m_listBox->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_3DFACE));
	PopulateListControl( static_cast<Notebook*>( parent ) );
	m_listBox->SetFocus();
}

void NotebookNavDialog::OnKeyUp(wxKeyEvent &event)
{
	if (event.GetKeyCode() == WXK_CONTROL) {
		CloseDialog();
	}
}

void NotebookNavDialog::OnNavigationKey(wxNavigationKeyEvent &event)
{
	long selected = m_listBox->GetSelection();
	long maxItems = m_listBox->GetCount();
	long itemToSelect;

	if ( event.GetDirection() ) {
		// Select next page
		if (selected == maxItems - 1)
			itemToSelect = 0;
		else
			itemToSelect = selected + 1;
	} else {
		// Previous page
		if ( selected == 0 )
			itemToSelect = maxItems - 1;
		else
			itemToSelect = selected - 1;
	}
	m_listBox->SetSelection( itemToSelect );
	m_listBox->SetFocus();
}

void NotebookNavDialog::PopulateListControl(Notebook *book)
{
	m_tabsIndex.clear();
	const wxArrayPtrVoid &arr = book->GetHistory();
	for (size_t i=0; i<arr.GetCount(); i++) {
		wxWindow *tab = static_cast<wxWindow*>(arr.Item(i));
		m_tabsIndex[m_listBox->Append( book->GetPageText( book->GetPageIndex(tab) ) )] = tab;
	}

	// Select the next entry after the current selection
	if (arr.GetCount() > 0) {
		m_listBox->SetSelection( 0 );
	}

	wxNavigationKeyEvent dummy;
	dummy.SetDirection(true);
	OnNavigationKey(dummy);
}

void NotebookNavDialog::OnItemSelected(wxCommandEvent & event )
{
	wxUnusedVar( event );
	CloseDialog();
}

void NotebookNavDialog::CloseDialog()
{
	m_selectedItem = m_listBox->GetSelection();
	m_selTab = NULL;
	std::map< int, wxWindow* >::iterator iter = m_tabsIndex.find(m_selectedItem);
	if(iter != m_tabsIndex.end()) {
		m_selTab = iter->second;
	}
	EndModal( wxID_OK );
}

void NotebookNavDialog::OnKeyDown(wxKeyEvent& event)
{
	if ( event.GetKeyCode() == WXK_TAB && event.ControlDown() && event.ShiftDown()) {
		// Navigate forward
		wxNavigationKeyEvent nav;
		nav.SetDirection(false);
		OnNavigationKey(nav);
		
		
	} else if ( event.GetKeyCode() == WXK_TAB && event.ControlDown() ) {
		// Navigate backward
		wxNavigationKeyEvent nav;
		nav.SetDirection(true);
		OnNavigationKey(nav);
		
	} else {
		event.Skip();
	}
}

