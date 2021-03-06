#include "php_workspace.h"
#include "php_strings.h"
#include <dirtraverser.h>
#include <plugin.h>
#include <globals.h>
#include <wx/busyinfo.h>
#include <wx/msgdlg.h>
#include <event_notifier.h>
#include <macros.h>
#include "php_event.h"
#include <cl_command_event.h>
#include <wx/msgdlg.h>
#include "php_code_completion.h"
#include "php_strings.h"
#include <wx/msgdlg.h>
#include "php_parser_thread.h"
#include <wx/progdlg.h>

#ifndef __WXMSW__
#include <errno.h>
#endif

PHPWorkspace* PHPWorkspace::ms_instance = 0;

PHPWorkspace::PHPWorkspace()
    : m_manager(NULL)
{
}

PHPWorkspace::~PHPWorkspace() { m_workspaceFile.Clear(); }

PHPWorkspace* PHPWorkspace::Get()
{
    if(ms_instance == 0) {
        ms_instance = new PHPWorkspace();
    }
    return ms_instance;
}

void PHPWorkspace::Release()
{
    if(ms_instance) {
        delete ms_instance;
    }
    ms_instance = 0;
}

bool PHPWorkspace::Close(bool saveBeforeClose)
{
    if(IsOpen()) {
        if(m_manager) {
            m_manager->StoreWorkspaceSession(m_workspaceFile);
        }
        if(saveBeforeClose) {
            // Save it
            Save();
        }
    }

    m_projects.clear();
    m_workspaceFile.Clear();

    // Close the code completion lookup table
    PHPCodeCompletion::Instance()->Close();

    PHPEvent phpEvent(wxEVT_PHP_WORKSPACE_CLOSED);
    EventNotifier::Get()->AddPendingEvent(phpEvent);

    // Notify that workspace has been opened to the plugins
    SendCmdEvent(wxEVT_WORKSPACE_CLOSED);
    return true;
}

bool PHPWorkspace::Open(const wxString& filename, bool createIfMissing)
{
    // Close the currently opened workspace
    if(IsOpen()) {
        Close();
    }

    m_workspaceFile = filename;
    wxFileName fnNewWspFile = m_workspaceFile;
    fnNewWspFile.SetExt("workspace");
    
    if(!fnNewWspFile.Exists()) {
        wxLogNull nolog;
        if(::wxCopyFile(m_workspaceFile.GetFullPath(), fnNewWspFile.GetFullPath())) {
            m_workspaceFile = fnNewWspFile;
        }
    }
    
    // Ensure that the workspace file is renamed to .workspace
    {
        // Create the private folder if needed
        wxFileName fn(m_workspaceFile);
        fn.AppendDir(".codelite");
        wxLogNull nolog;
        ::wxMkdir(fn.GetPath());
    }

    if(!m_workspaceFile.FileExists()) {
        if(createIfMissing) {
            if(!Create(filename)) {
                return false;
            }
        } else {
            m_workspaceFile.Clear();
            return false;
        }
    }

    // point the tags storage to the correct db file
    wxFileName tagsfile(filename);
    tagsfile.SetExt(wxT("phptags"));

    // set the working directory to the workspace path
    ::wxSetWorkingDirectory(m_workspaceFile.GetPath());

    JSONRoot root(m_workspaceFile);
    FromJSON(root.toElement());

    // We open the symbols database manually here and _not_ via an event
    // since the parser thread might open it first and leave us with a lock
    PHPCodeCompletion::Instance()->Open(m_workspaceFile);

    // Notify internally that the workspace is loaded
    PHPEvent phpEvent(wxEVT_PHP_WORKSPACE_LOADED);
    phpEvent.SetFileName(m_workspaceFile.GetFullPath());
    EventNotifier::Get()->AddPendingEvent(phpEvent);

    // Notify that the a new workspace is loaded
    // This time send the standard codelite event
    // this is important so other plugins such as Svn, Git
    // want to adjust their paths according to the new workspace
    {
        wxCommandEvent event(wxEVT_WORKSPACE_LOADED);
        event.SetString(GetFilename().GetFullPath());
        EventNotifier::Get()->AddPendingEvent(event);
    }
    
    wxBusyInfo busy(_("Scanning for workspace files..."), EventNotifier::Get()->TopFrame());
    wxYieldIfNeeded();
    
    wxArrayString dummy;
    GetWorkspaceFiles(dummy);
    
    // progress->Destroy();
    
    // Perform a quick re-parse of the workspace
    ParseWorkspace(false);

    CallAfter(&PHPWorkspace::RestoreWorkspaceSession);
    
    // Change the workspace extension
    return true;
}

bool PHPWorkspace::IsOpen() const { return m_workspaceFile.IsOk() && m_workspaceFile.FileExists(); }

void PHPWorkspace::CreateProject(const PHPProject::CreateData& createData)
{
    wxString projectName;
    wxFileName fnProjectFileName(createData.path, "");
    projectName << createData.name << ".phprj";
    fnProjectFileName.SetFullName(projectName);

    if(HasProject(projectName)) return;
    
    // Ensure that the path to the file exists
    fnProjectFileName.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    
    // Create an empty project and initialize it with the global settings
    PHPProject::Ptr_t proj(new PHPProject());
    // Setup the file path + name
    proj->Create(fnProjectFileName, createData.name);
    proj->GetSettings().MergeWithGlobalSettings();
    if(!createData.phpExe.IsEmpty() && wxFileName::Exists(createData.phpExe)) {
        proj->GetSettings().SetPhpExe(createData.phpExe);
    }
    proj->GetSettings().SetRunAs(createData.projectType);

    m_projects.insert(std::make_pair(proj->GetName(), proj));
    if(m_projects.size() == 1) {
        SetProjectActive(proj->GetName());
    }
    Save();
    proj->Save();
}

bool PHPWorkspace::IsProjectExists(const wxString& project) { return HasProject(project); }

const PHPProject::Map_t& PHPWorkspace::GetProjects() const { return m_projects; }

void PHPWorkspace::DeleteProject(const wxString& project)
{
    PHPProject::Ptr_t p = GetProject(project);
    CHECK_PTR_RET(p);
    m_projects.erase(project);
    Save(); // save the workspace
}

void PHPWorkspace::SetProjectActive(const wxString& project)
{
    PHPProject::Map_t::iterator iter = m_projects.begin();
    for(; iter != m_projects.end(); ++iter) {
        iter->second->SetIsActive(iter->first == project);
    }
}

void PHPWorkspace::GetWorkspaceFiles(wxStringSet_t& workspaceFiles, wxProgressDialog* progress) const
{
    PHPProject::Map_t::const_iterator iter = m_projects.begin();
    for(; iter != m_projects.end(); ++iter) {
        const wxArrayString& files = iter->second->GetFiles(progress);
        workspaceFiles.insert(files.begin(), files.end());
    }
}

void PHPWorkspace::GetWorkspaceFiles(wxStringSet_t& workspaceFiles, const wxString& filter) const
{
    wxStringSet_t all_files;
    GetWorkspaceFiles(all_files);
    if(filter.IsEmpty()) {
        workspaceFiles.swap(all_files);

    } else {
        wxStringSet_t::iterator iter = all_files.begin();
        for(; iter != all_files.end(); ++iter) {
            wxFileName fn(*iter);
            if(fn.GetName().Contains(filter)) {
                workspaceFiles.insert(*iter);
            }
        }
    }
}

void PHPWorkspace::DoNotifyFilesRemoved(const wxArrayString& files)
{
    if(!files.IsEmpty()) {

        wxBusyInfo info(_("Updating workspace..."));
        wxYieldIfNeeded();
        EventNotifier::Get()->PostFileRemovedEvent(files);
    }
}

wxString PHPWorkspace::GetActiveProjectName() const
{
    PHPProject::Map_t::const_iterator iter = m_projects.begin();
    for(; iter != m_projects.end(); ++iter) {
        if(iter->second->IsActive()) {
            return iter->second->GetName();
        }
    }
    return "";
}

void PHPWorkspace::DelFile(const wxString& project, const wxString& filename)
{
    PHPProject::Ptr_t proj = GetProject(project);
    CHECK_PTR_RET(proj);

    wxArrayString files;
    files.Add(filename);
    proj->FilesDeleted(files, true);
}

bool PHPWorkspace::RunProject(bool debugging,
                              const wxString& urlOrFilePath,
                              const wxString& projectName,
                              const wxString& xdebugSessionName)
{
    wxString projectToRun = projectName;
    if(projectToRun.IsEmpty()) {
        projectToRun = GetActiveProjectName();
    }

    PHPProject::Ptr_t proj = PHPWorkspace::Get()->GetProject(projectToRun);
    CHECK_PTR_RET_FALSE(proj);
    // Error is reported inside 'Exec'
    return m_executor.Exec(projectToRun, urlOrFilePath, xdebugSessionName, debugging);
}

bool PHPWorkspace::HasProject(const wxString& projectname) const
{
    if(!IsOpen()) {
        return false;
    }
    return m_projects.count(projectname);
}

void PHPWorkspace::FromJSON(const JSONElement& e)
{
    m_projects.clear();
    if(e.hasNamedObject("projects")) {
        JSONElement projects = e.namedObject("projects");
        int count = projects.arraySize();
        for(int i = 0; i < count; ++i) {
            PHPProject::Ptr_t p(new PHPProject());
            wxString project_file = projects.arrayItem(i).toString();
            wxFileName fnProject(project_file);
            fnProject.MakeAbsolute(m_workspaceFile.GetPath());
            p->Load(fnProject);
            m_projects.insert(std::make_pair(p->GetName(), p));
        }
    }
}

#define PHP_WORKSPACE_VERSION wxString("1.0")
#define PHP_WORKSPACE_IDE wxString("CodeLite")

JSONElement PHPWorkspace::ToJSON(JSONElement& e) const
{
    JSONElement metadata = JSONElement::createObject("metadata");
    e.append(metadata);
    
    metadata.addProperty("version", PHP_WORKSPACE_VERSION);
    metadata.addProperty("ide", PHP_WORKSPACE_IDE);
    metadata.addProperty("type", wxString("php"));
    
    // Store the list of files
    JSONElement projectsArr = JSONElement::createArray("projects");
    e.append(projectsArr);

    PHPProject::Map_t::const_iterator iter = m_projects.begin();
    for(; iter != m_projects.end(); ++iter) {
        wxFileName projectFile = iter->second->GetFilename();
        projectFile.MakeRelativeTo(m_workspaceFile.GetPath());
        projectsArr.arrayAppend(projectFile.GetFullPath(wxPATH_UNIX));
    }
    
    
    return e;
}

PHPProject::Ptr_t PHPWorkspace::GetActiveProject() const { return GetProject(GetActiveProjectName()); }

PHPProject::Ptr_t PHPWorkspace::GetProject(const wxString& project) const
{
    if(!HasProject(project)) {
        return PHPProject::Ptr_t(NULL);
    }
    return m_projects.find(project)->second;
}

void PHPWorkspace::Save()
{
    if(!IsOpen()) {
        return;
    }
    // seriaize the workspace and store it to disk
    JSONRoot root(cJSON_Object);
    JSONElement ele = root.toElement();
    ToJSON(ele);
    root.save(m_workspaceFile);
}

bool PHPWorkspace::Create(const wxString& filename)
{
    {
        wxFileName fn(filename);
        fn.AppendDir(".codelite");
        wxLogNull nolog;
        ::wxMkdir(fn.GetPath());
    }

    wxFileName fn(filename);

    if(fn.FileExists()) {
        return true;
    }

    // create it
    JSONRoot root(cJSON_Object);
    JSONElement ele = root.toElement();
    ToJSON(ele);
    root.save(fn);
    return true;
}

wxString PHPWorkspace::GetPrivateFolder() const
{
    wxFileName fn(GetFilename());
    fn.AppendDir(".codelite");
    return fn.GetPath();
}

void PHPWorkspace::Rename(const wxString& newname)
{
    wxFileName new_path(GetFilename());
    new_path.SetName(newname);

    if(!::wxRenameFile(m_workspaceFile.GetFullPath(), new_path.GetFullPath())) {
        wxString msg;
        msg << _("Failed to rename workspace file:\n'") << m_workspaceFile.GetFullName() << _("'\nto:\n'")
            << new_path.GetFullName() << "'\n" << strerror(errno);
        ::wxMessageBox(msg, "CodeLite", wxOK | wxCENTER | wxICON_ERROR, EventNotifier::Get()->TopFrame());
        return;
    }

    PHPEvent evt(wxEVT_PHP_WORKSPACE_RENAMED);
    evt.SetOldFilename(m_workspaceFile.GetFullPath());
    evt.SetFileName(new_path.GetFullPath());
    EventNotifier::Get()->AddPendingEvent(evt);

    // and now rename the actual file
    m_workspaceFile = new_path;

    // trigger a workspace parsing
    wxCommandEvent evtRetag(wxEVT_CMD_RETAG_WORKSPACE_FULL);
    EventNotifier::Get()->AddPendingEvent(evtRetag);
}

void PHPWorkspace::DoPromptWorkspaceModifiedDialog()
{
    wxMessageDialog dlg(FRAME,
                        _("Workspace file modified externally. Would you like to reload the workspace?"),
                        "CodeLite",
                        wxYES_NO | wxCENTER);
    dlg.SetYesNoLabels(_("Reload Workspace"), _("Ignore"));

    int answer = dlg.ShowModal();
    if(answer == wxID_YES) {
        wxCommandEvent evtReload(wxEVT_COMMAND_MENU_SELECTED, XRCID("reload_workspace"));
        FRAME->GetEventHandler()->AddPendingEvent(evtReload);
    }
}

void PHPWorkspace::GetWorkspaceFiles(wxArrayString& workspaceFiles, wxProgressDialog* progress) const
{
    wxStringSet_t files;
    GetWorkspaceFiles(files, progress);
    workspaceFiles.clear();
    wxStringSet_t::const_iterator iter = files.begin();
    for(; iter != files.end(); ++iter) {
        workspaceFiles.Add(*iter);
    }
}

void PHPWorkspace::ParseWorkspace(bool full)
{
    // Request for parsing
    PHPParserThreadRequest* req = new PHPParserThreadRequest(full ? PHPParserThreadRequest::kParseWorkspaceFilesFull :
                                                                    PHPParserThreadRequest::kParseWorkspaceFilesQuick);
    req->workspaceFile = GetFilename().GetFullPath();
    GetWorkspaceFiles(req->files);

    // Append the current project CC include paths
    PHPProject::Ptr_t pProject = GetActiveProject();
    if(pProject) {
        PHPProjectSettingsData& settings = pProject->GetSettings();
        req->frameworksPaths = settings.GetCCIncludePathAsArray();
    }
    PHPParserThread::Instance()->Add(req);
}

TerminalEmulator* PHPWorkspace::GetTerminalEmulator() { return m_executor.GetTerminalEmulator(); }

void PHPWorkspace::RestoreWorkspaceSession()
{
    if(m_manager && IsOpen()) {
        m_manager->LoadWorkspaceSession(m_workspaceFile);
    }
}

void PHPWorkspace::SyncWithFileSystem()
{
    PHPProject::Map_t::const_iterator iter = m_projects.begin();
    for(; iter != m_projects.end(); ++iter) {
        iter->second->SynchWithFileSystem();
    }
}

PHPProject::Ptr_t PHPWorkspace::GetProjectForFile(const wxFileName& filename) const
{
    PHPProject::Map_t::const_iterator iter = m_projects.begin();
    for(; iter != m_projects.end(); ++iter) {
        if(iter->second->HasFile(filename)) return iter->second;
    }
    return PHPProject::Ptr_t(NULL);
}
