#include "common.h"
#include "store.h"
#include "HdfsSync.h"

#ifdef USE_SCRIBE_HDFS_SYNC
#include "hdfs.h"

using namespace std;
using namespace boost;

const string meta_logfile_prefix = "scribe_meta<new_logfile>: ";

HdfsSync::HdfsSync(const string& category, bool multi_category,
                     bool is_buffer_file)
  : FileStore(category,multi_category,is_buffer_file),
  hdfsHost(""),hdfsPort(0),hdfsPath("/"),hdfs_base_filename(""),hdfs_base_directory("")
{

}

HdfsSync::~HdfsSync() 
{
    
}

void HdfsSync::configure(pStoreConf configuration) {

  FileStore::configure(configuration);

  // Hdfs Sync doesn't use chunk file
  rollPeriod = ROLL_NEVER;
  chunkSize = 0;

  unsigned long inttemp = 0;
  configuration->getUnsigned("add_newlines", inttemp);
  addNewlines = inttemp ? true : false;

  lastSyncTime = time (NULL);

  configuration->getUnsigned("period_length", periodLength);  
 
  configuration->getString("hdfs_dir", hdfsDir);  
  if(configuration->getString("hdfs_base_filename", hdfs_base_filename)) {
    hdfs_base_filename.append("_");
  }
  if(configuration->getString("hdfs_base_directory", hdfs_base_directory)) {
    hdfs_base_directory.append("/");
  }
  
  // Parse HDFS information
  char* hostport = (char *)malloc(hdfsDir.length()+1);
  char* buf;
  char* portStr;
  char* host;

  int ret = 0;

  ret = sscanf(hdfsDir.c_str(), "hdfs://%s", hostport);
  host = strtok_r(hostport, ":", &buf);
  portStr = strtok_r(NULL, "/", &buf);

  ret = sscanf(portStr, "%d", &hdfsPort);
  hdfsHost.append(host);
  hdfsPath.append(buf);
  free(hostport);  
}

void HdfsSync::copyToHDFS()
{
  vector<std::string>::iterator it = filesToCopy.begin();
  while (it!=filesToCopy.end())
  {
    bool success = false;
    std::string filename = *it;
	std::string baseFilename = filename;

    int found = filename.find_last_of("/");

    if (found != string::npos)
    {
      baseFilename = filename.substr(found+1);
    }    

    hdfsFS fs = hdfsConnect(hdfsHost.c_str(), hdfsPort);
    if (NULL == fs ) 
    {
      LOG_OPER("Cannot connect to hdfs://%s:%d",hdfsHost.c_str(), hdfsPort);
    }
    else
    {
      string writePath = hdfsPath + "/" + categoryHandled.c_str() + "/" + hdfs_base_directory.c_str() + hdfs_base_filename.c_str() + baseFilename;              
      ifstream file_in(filename.c_str(), ios::in | ios::binary);
      hdfsFile dstFile = hdfsOpenFile(fs, writePath.c_str(), O_WRONLY, 0, 0, 0);
	  if (NULL == dstFile)
      {
          LOG_OPER("Cannot open file to write : %s",writePath.c_str());
      }
      else
      {
        char buffer[1024];
        while (!file_in.eof())
        {
          file_in.read(buffer, 1024);
          tSize num_written_bytes = hdfsWrite(fs, dstFile, (void*)buffer, file_in.gcount());      
        }
        file_in.close();
        hdfsCloseFile(fs, dstFile);
        
        LOG_OPER("Copied to HDFS hdfs://%s:%d/%s",hdfsHost.c_str(),hdfsPort,writePath.c_str()); 
        success = true;
        
      }
      hdfsDisconnect(fs);
    } // end else
    if (success) {
      filesToCopy.erase(it);
      unlink(filename.c_str());
    }
    else
      it++;
  } // end while
}

bool HdfsSync::openInternal(bool incrementFilename, struct tm* current_time) {
  bool success = false;
  LOG_OPER("OPEN INTERNAL");
  if (!current_time) {
    time_t rawtime;
    time(&rawtime);
    current_time = localtime(&rawtime);
  }
  try {
    std::string base_filename = makeBaseFilename(current_time);
    int suffix = findNewestFile(base_filename);
    int suffix1 = -1;

    if (filesToCopy.size()==0) {
      std::vector<std::string> files = FileInterface::list(filePath, "std");
      for (std::vector<std::string>::reverse_iterator iter = files.rbegin();
           iter != files.rend();
           ++iter) {
        suffix1 = getFileSuffix(*iter, base_filename);
        if (suffix1 > -1 && suffix1 != suffix) {
           ostringstream filename;
           filename << filePath << '/';
           filename << *iter;
           filesToCopy.push_back(filename.str());
        }
      }
    }
      
    if (incrementFilename) {
      ++suffix;
    }

    // this is the case where there's no file there and we're not incrementing
    if (suffix < 0) {
      suffix = 0;
    }

    string file = makeFullFilename(suffix, current_time);

    if (rollPeriod == ROLL_DAILY) {
      lastRollTime = current_time->tm_mday;
    } else {  // default to hourly if rollPeriod is garbage
      lastRollTime = current_time->tm_hour;
    }

    bool sync = false;
    string fullFilename;
    string baseFilename;
      
    // Save old file
    if (writeFile) {
      if (writeMeta) {
        writeFile->write(meta_logfile_prefix + file);
      }

      if(writeFile->fileSize())
        sync = true;
      
      fullFilename = writeFile->getFileName();
      baseFilename = fullFilename;
      int found = fullFilename.find_last_of("/");
      if (found != string::npos)
      {
        baseFilename = fullFilename.substr(found+1);
      }      
   
      writeFile->close();
    }

    // Open new file
    writeFile = FileInterface::createFileInterface(fsType, file, isBufferFile);

    copyToHDFS();
    
    if (!writeFile) {
      LOG_OPER("[%s] Failed to create file <%s> of type <%s> for writing", 
               categoryHandled.c_str(), file.c_str(), fsType.c_str());
      setStatus("file open error");
      return false;
    }
    
    success = writeFile->openWrite();    

    if (!success) {
      LOG_OPER("[%s] Failed to open file <%s> for writing", categoryHandled.c_str(), file.c_str());
      setStatus("File open error");
    } else {

    /* just make a best effort here, and don't error if it fails */
    if (createSymlink && !isBufferFile) {
      string symlinkName = makeFullSymlink();
      unlink(symlinkName.c_str());
      symlink(file.c_str(), symlinkName.c_str());
    }
    // else it confuses the filename code on reads

	// Open successful, add file to queue
    filesToCopy.push_back(file);

    LOG_OPER("[%s] Opened file <%s> for writing", categoryHandled.c_str(), file.c_str());


    currentSize = writeFile->fileSize();
    currentFilename = file;
    eventsWritten = 0;
    setStatus("");                

    }
  } catch(std::exception const& e) {
    LOG_OPER("[%s] Failed to create/open file of type <%s> for writing",
             categoryHandled.c_str(), fsType.c_str());
    LOG_OPER("Exception: %s", e.what());
    setStatus("file create/open error");
     
    return false;
  }
  return success;
}

void HdfsSync::periodicCheck() {

  time_t currentTime;
  time(&currentTime);

  if (!lastSyncTime) 
  {
    lastSyncTime = currentTime;
  }

  if ((currentTime - lastSyncTime) / 60 >= periodLength)
  {            
    struct tm *timeinfo;
    timeinfo = localtime(&currentTime);
    rotateFile(timeinfo);
    lastSyncTime = currentTime;
  }
}

boost::shared_ptr<Store> HdfsSync::copy(const std::string &category) {
  HdfsSync *store = new HdfsSync(category, multiCategory, isBufferFile);
  shared_ptr<Store> copied = shared_ptr<Store>(store);

  store->addNewlines = addNewlines;
  store->periodLength = periodLength;
  store->hdfsHost = hdfsHost;
  store->hdfsPort = hdfsPort;
  store->hdfsPath = hdfsPath;
  store->hdfs_base_filename = hdfs_base_filename;
  store->hdfs_base_directory = hdfs_base_directory;
  store->copyCommon(this);
  return copied;
}

#endif // USE_SCRIBE_HDFS_SYNC
