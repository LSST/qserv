from  lsst.qserv.admin import commons
import logging
import os
import re
import shutil
import SQLCmd
import SQLConnection
import SQLInterface
import SQLMode

class QservDataLoader():

    def __init__(self, config, db_name, out_dirname, log_file_prefix='qserv-loader', logging_level=logging.DEBUG):
        self.config = config

        self._dbName = db_name

        self._out_dirname = out_dirname

        #self.logger = commons.console_logger(logging_level)
        #self.logger = commons.file_logger(
        #    log_file_prefix,
        #    log_path=self.config['qserv']['log_dir']
        #)
        self.logger = logging.getLogger()
        sock_connection_params = {
            'config' : self.config,
            'mode' : SQLMode.MYSQL_SOCK,
            'database' : self._dbName
            }

        self._sqlInterface = dict()
        self._sqlInterface['sock'] = SQLConnection.SQLConnection(**sock_connection_params)
        self._sqlInterface['cmd'] = SQLCmd.SQLCmd(**sock_connection_params)

       

    def getDataConfig(self):
        data_config = dict()
        data_config['Object']=dict()
        data_config['Object']['ra-column'] = self._schemaDict['Object'].indexOf("`ra_PS`")
        data_config['Object']['decl-column'] = self._schemaDict['Object'].indexOf("`decl_PS`")
        
        # zero-based index

        # FIXME : return 229 instead of 227
        #data_config['Object']['chunk-column-id'] = self._schemaDict['Object'].indexOf("`chunkId`") -2
        
        # for test case01
        #data_config['Object']['ra-column'] = 2
        #data_config['Object']['decl-column'] = 4
        data_config['Object']['chunk-column-id'] = 227

        data_config['Source']=dict()
        # Source will be placed on the same chunk that its related Object
        data_config['Source']['ra-column'] = self._schemaDict['Source'].indexOf("`raObject`")
        data_config['Source']['decl-column'] = self._schemaDict['Source'].indexOf("`declObject`")

        data_config['out-dirname'] = '/data/'

        # for test case01
        #data_config['Source']['ra-column'] = 33
        #data_config['Source']['decl-column'] = 34

        # chunkId and subChunkId will be added
        data_config['Source']['chunk-column-id'] = None

        self.logger.debug("Data configuration : %s" % data_config)
        return data_config

    def workerGetNonEmptyChunkIds(self):
        non_empty_chunk_list=[]

        sql = "SHOW TABLES IN %s LIKE \"Object\_%%\";" % self._dbName 
        rows = self._sqlInterface['sock'].execute(sql)

        for row in rows:
            self.logger.debug("Chunk table found : %s" % row)
            pattern = re.compile(r"^Object_([0-9]+)$")
            m = pattern.match(row[0])
            if m:
                chunk_id = m.group(1)
                non_empty_chunk_list.append(int(chunk_id))
                self.logger.debug("Chunk number : %s" % chunk_id)
        chunk_list = sorted(non_empty_chunk_list)
        self.logger.info("Non empty data chunks list : %s " %  chunk_list)
        return chunk_list
        
    def initDatabases(self): 
        self.logger.info("Initializing databases %s, qservMeta" % self._dbName)
        sql_instructions= [
            "DROP DATABASE IF EXISTS %s" % self._dbName,
            "CREATE DATABASE %s" % self._dbName,
            # TODO : "GRANT ALL ON %s.* TO '%s'@'*'" % (self._dbName, self._qservUser, self._qservHost)
            "GRANT ALL ON %s.* TO '*'@'*'" % (self._dbName),
            "DROP DATABASE IF EXISTS qservMeta",
            "CREATE DATABASE qservMeta",
            "USE %s" %  self._dbName
            ]
        
        for sql in sql_instructions:
            self._sqlInterface['sock'].execute(sql)

    def masterCreateEmptyChunksFile(self, stripes, chunk_id_list, empty_chunks_filename):
        f=open(empty_chunks_filename,"w")
        empty_chunks_list=[i for i in range(0,7201) if i not in chunk_id_list]
        for i in empty_chunks_list:
            f.write("%s\n" %i)
        f.close()

    def workerCreateXrootdExportDirs(self, non_empty_chunk_id_list):

        # match oss.localroot in etc/lsp.cf
        xrootd_run_dir = os.path.join(self.config['qserv']['base_dir'],'xrootd-run')

        # TODO : read 'q' and 'result' in etc/lsp.cf
        xrd_query_dir = os.path.join(xrootd_run_dir, 'q', self._dbName) 
        xrd_result_dir = os.path.join(xrootd_run_dir, 'result') 

        if os.path.exists(xrd_query_dir):
            self.logger.info("Emptying existing xrootd query dir : %s" % xrd_query_dir)
            shutil.rmtree(xrd_query_dir)
        os.makedirs(xrd_query_dir)
        self.logger.info("Making placeholders")

        for chunk_id in non_empty_chunk_id_list:
            xrd_file = os.path.join(xrd_query_dir,str(chunk_id))
            open(xrd_file, 'w').close() 

        if os.path.exists(xrd_result_dir):
            self.logger.info("Emptying existing xrootd result dir : %s" % xrd_result_dir)
            shutil.rmtree(xrd_result_dir)
        os.makedirs(xrd_result_dir)


    def loadPartitionedTable(self, table, schemaFile, data_filename):
        ''' Partition and load Qserv data like Source and Object
        '''

        data_config = self.getDataConfig()
        
        # load schema with chunkId and subChunkId
        self.logger.info("  Loading schema %s" % schemaFile)
        self._sqlInterface['sock'].executeFromFile(schemaFile)
        # TODO : create index and alter table with chunkId and subChunkId
        # "\nCREATE INDEX obj_objectid_idx on Object ( objectId );\n";

        partition_dirname = self.partitionData(data_config,table,data_filename)
        
        self.loadPartitionedData(partition_dirname,table)

        self.workerCreateTable1234567890(table)

        chunk_id_list=self.workerGetNonEmptyChunkIds()
        self.masterCreateAndFeedMetaTable(table,chunk_id_list)

        # Create xrootd query directories
        self.workerCreateXrootdExportDirs(chunk_id_list)

        # Create etc/emptychunk.txt
        empty_chunks_filename = os.path.join(self.config['qserv']['base_dir'],"etc","emptyChunks.txt")
        stripes=self.config['qserv']['stripes']
        self.masterCreateEmptyChunksFile(stripes, chunk_id_list,  empty_chunks_filename)

        raw_input("Qserv mono-node database filled with partitionned '%s' data.\nPress Enter to continue..." % table)


    def partitionData(self,data_config,table, data_filename):
        # partition data          
        
        partition_scriptname = os.path.join(self.config['qserv']['base_dir'],"qserv", "master", "examples", "partition.py")
        partition_dirname = os.path.join(self._out_dirname,table+"_partition")
        if os.path.exists(partition_dirname):
            shutil.rmtree(partition_dirname)
        os.makedirs(partition_dirname)
            
            # python %s -PObject -t 2  -p 4 %s --delimiter '\t' -S 10 -s 2 --output-dir %s" % (self.partition_scriptname, data_filename, partition_dirname
        partition_data_cmd = [
            self.config['bin']['python'],
            partition_scriptname,
            '--output-dir', partition_dirname,
            '--chunk-prefix', table,
            '--theta-column', str(data_config[table]['ra-column']),
            '--phi-column', str(data_config[table]['decl-column']),
            '--num-stripes', self.config['qserv']['stripes'],
            '--num-sub-stripes', self.config['qserv']['substripes'],
            '--delimiter', '\t'
            ]
        
        if data_config[table]['chunk-column-id'] != None :
            partition_data_cmd.extend(['--chunk-column', str(data_config[table]['chunk-column-id'])])
            
        partition_data_cmd.append(data_filename)
            
        out = commons.run_command(partition_data_cmd)
        
        self.logger.info("Working in DB : %s.  LSST %s data partitioned : \n %s"
                % (self._dbName, table,out))

        return partition_dirname


    def loadPartitionedData(self,partition_dirname,table):

        load_scriptname = os.path.join(self.config['qserv']['base_dir'],"qserv", "master", "examples", "loader.py")

    # TODO : remove hard-coded param : qservTest_caseXX_mysql => check why model table already exists in self._dbName
        load_partitionned_data_cmd = [
            self.config['bin']['python'], 
            load_scriptname,
            '--user=%s' % self.config['mysqld']['user'], 
            '--password=%s' % self.config['mysqld']['pass'],
            '--database=%s' % self._dbName,
            "%s:%s" %
            (self.config['qserv']['master'],self.config['mysqld']['port']),
            partition_dirname,
            "%s.%s" % (self._dbName, table)
            ]
        # python master/examples/loader.py --verbose -u root -p changeme --database qservTest_case01_qserv -D clrlsst-dbmaster.in2p3.fr:13306 /opt/qserv-dev/tmp/Object_partition/ qservTest_case01_mysql.Object
        out = commons.run_command(load_partitionned_data_cmd)
        self.logger.info("Partitioned %s data loaded : %s" % (table,out))

    def workerCreateTable1234567890(self,table):
        sql =  "CREATE TABLE {0}.{1}_1234567890 LIKE {1};\n".format(self._dbName,table)
        self._sqlInterface['sock'].execute(sql)

        self.logger.info("%s table for empty chunk created" % table)

    def masterCreateAndFeedMetaTable(self,table,chunk_id_list):

        sql = "USE qservMeta;"
        sql += "CREATE TABLE LSST__{0} ({1}Id BIGINT NOT NULL PRIMARY KEY, x_chunkId INT, x_subChunkId INT);\n".format(table, table.lower())

        # TODO : scan data on all workers here, with recovery on error
        insert_sql =  "INSERT INTO LSST__{1} SELECT {2}Id, chunkId, subChunkId FROM {0}.{1}_%s;".format(self._dbName,table,table.lower())
        for chunkId in chunk_id_list :
            tmp =  insert_sql % chunkId
            sql += "\n" + tmp

        self._sqlInterface['sock'].execute(sql)
        self.logger.info("meta table created and loaded for %s" % table)
