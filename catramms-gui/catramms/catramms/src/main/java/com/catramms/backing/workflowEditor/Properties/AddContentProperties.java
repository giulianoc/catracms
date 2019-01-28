package com.catramms.backing.workflowEditor.Properties;

import com.catramms.backing.common.SessionUtils;
import com.catramms.backing.workflowEditor.utility.PushContent;
import com.catramms.backing.workflowEditor.utility.WorkflowIssue;
import com.catramms.backing.workflowEditor.utility.IngestionData;
import org.apache.commons.io.IOUtils;
import org.apache.log4j.Logger;
import org.json.JSONObject;
import org.primefaces.event.FileUploadEvent;

import java.io.*;
import java.text.DateFormat;
import java.text.SimpleDateFormat;
import java.util.TimeZone;

public class AddContentProperties extends CreateContentProperties implements Serializable {

    private static final Logger mLogger = Logger.getLogger(AddContentProperties.class);

    private String sourceDownloadType;
    private String pullSourceURL;
    private String pushBinaryFileName;
    private String fileFormat;
    private String md5FileChecksum;
    private Long fileSizeInBytes;

    private String temporaryPushBinariesPathName;
    private String videoAudioAllowTypes;

    public AddContentProperties(String positionX, String positionY,
                                int elementId, String label, String temporaryPushBinariesPathName)
    {
        super(positionX, positionY, elementId, label, "Add-Content" + "-icon.png", "Task", "Add-Content");

        sourceDownloadType = "pull";

        this.temporaryPushBinariesPathName = temporaryPushBinariesPathName;

        {
            String fileExtension = "";   // = "wmv|mp4|ts|mpeg|avi|webm|mp3|aac|png|jpg";

            for (String fileFormat: getFileFormatsList())
            {
                if (fileExtension.isEmpty())
                    fileExtension = fileFormat;
                else
                    fileExtension += ("|" + fileFormat);
            }

            videoAudioAllowTypes = ("/(\\.|\\/)(" + fileExtension + ")$/");
        }
    }

    public AddContentProperties clone()
    {
        AddContentProperties addContentProperties = new AddContentProperties(
                super.getPositionX(), super.getPositionY(),
                super.getElementId(), super.getLabel(), getTemporaryPushBinariesPathName());

        mLogger.info("AddContentProperties clone"
                + ", sourceDownloadType: " + sourceDownloadType
                + ", pullSourceURL: " + pullSourceURL
        );

        addContentProperties.setSourceDownloadType(sourceDownloadType);
        addContentProperties.setPullSourceURL(pullSourceURL);
        addContentProperties.setPushBinaryFileName(pushBinaryFileName);
        addContentProperties.setFileFormat(fileFormat);
        addContentProperties.setMd5FileChecksum(md5FileChecksum);
        addContentProperties.setFileSizeInBytes(fileSizeInBytes);

        super.getData(addContentProperties);


        return addContentProperties;
    }

    public void setData(AddContentProperties workflowProperties)
    {
        super.setData(workflowProperties);

        mLogger.info("AddContentProperties setData"
                + ", workflowProperties.getSourceDownloadType: " + workflowProperties.getSourceDownloadType()
                + ", workflowProperties.getPullSourceURL: " + workflowProperties.getPullSourceURL()
        );

        setSourceDownloadType(workflowProperties.getSourceDownloadType());
        setPullSourceURL(workflowProperties.getPullSourceURL());
        setPushBinaryFileName(workflowProperties.getPushBinaryFileName());
        setFileFormat(workflowProperties.getFileFormat());
        setMd5FileChecksum(workflowProperties.getMd5FileChecksum());
        setFileSizeInBytes(workflowProperties.getFileSizeInBytes());
    }

    public void setData(JSONObject jsonWorkflowElement)
    {
        try {
            super.setData(jsonWorkflowElement);

            setLabel(jsonWorkflowElement.getString("Label"));

            JSONObject joParameters = jsonWorkflowElement.getJSONObject("Parameters");

            if (joParameters.has("SourceURL") && !joParameters.getString("SourceURL").equalsIgnoreCase(""))
                setSourceDownloadType("pull");
            else
                setSourceDownloadType("push");

            // setPushBinaryFileName(workflowProperties.getPushBinaryFileName());
            if (joParameters.has("FileFormat") && !joParameters.getString("FileFormat").equalsIgnoreCase(""))
                setFileFormat(joParameters.getString("FileFormat"));
            if (joParameters.has("MD5FileChecksum") && !joParameters.getString("MD5FileChecksum").equalsIgnoreCase(""))
                setMd5FileChecksum(joParameters.getString("MD5FileChecksum"));
            if (joParameters.has("FileSizeInBytes"))
                setFileSizeInBytes(joParameters.getLong("FileSizeInBytes"));
        }
        catch (Exception e)
        {
            mLogger.error("WorkflowProperties:setData failed, exception: " + e);
        }
    }

    public JSONObject buildWorkflowElementJson(IngestionData ingestionData)
            throws Exception
    {
        JSONObject jsonWorkflowElement = new JSONObject();

        try
        {
            DateFormat dateFormat = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'");
            dateFormat.setTimeZone(TimeZone.getTimeZone("GMT"));

            jsonWorkflowElement.put("Type", super.getType());

            JSONObject joParameters = new JSONObject();
            jsonWorkflowElement.put("Parameters", joParameters);

            mLogger.info("AddContentProperties buildWorkflowElementJson"
                    + ", getSourceDownloadType(): " + getSourceDownloadType()
                    + ", getPullSourceURL(): " + getPullSourceURL()
            );

            if (super.getLabel() != null && !super.getLabel().equalsIgnoreCase(""))
                jsonWorkflowElement.put("Label", super.getLabel());
            else
            {
                WorkflowIssue workflowIssue = new WorkflowIssue();
                workflowIssue.setLabel("");
                workflowIssue.setFieldName("Label");
                workflowIssue.setTaskType(super.getType());
                workflowIssue.setIssue("The field is not initialized");

                ingestionData.getWorkflowIssueList().add(workflowIssue);
            }

            if (getSourceDownloadType().equalsIgnoreCase("pull"))
            {
                if (getPullSourceURL() != null && !getPullSourceURL().equalsIgnoreCase(""))
                    joParameters.put("SourceURL", getPullSourceURL());
                else
                {
                    WorkflowIssue workflowIssue = new WorkflowIssue();
                    workflowIssue.setLabel(super.getLabel());
                    workflowIssue.setFieldName("SourceURL");
                    workflowIssue.setTaskType(super.getType());
                    workflowIssue.setIssue("The field is not initialized");

                    ingestionData.getWorkflowIssueList().add(workflowIssue);
                }
            }
            else
            {
                if (getPushBinaryFileName() == null || getPushBinaryFileName().equalsIgnoreCase(""))
                {
                    WorkflowIssue workflowIssue = new WorkflowIssue();
                    workflowIssue.setLabel(super.getLabel());
                    workflowIssue.setFieldName("File");
                    workflowIssue.setTaskType(super.getType());
                    workflowIssue.setIssue("The field is not initialized");

                    ingestionData.getWorkflowIssueList().add(workflowIssue);
                }
                else
                {
                    PushContent pushContent = new PushContent();
                    pushContent.setLabel(super.getLabel());
                    pushContent.setBinaryPathName(getLocalBinaryPathName(getPushBinaryFileName()));

                    ingestionData.getPushContentList().add(pushContent);
                }
            }
            joParameters.put("FileFormat", getFileFormat());
            super.addCreateContentPropertiesToJson(joParameters);
            if (getMd5FileChecksum() != null && !getMd5FileChecksum().equalsIgnoreCase(""))
                joParameters.put("MD5FileChecksum", getMd5FileChecksum());
            if (getFileSizeInBytes() != null)
                joParameters.put("FileSizeInBytes", getFileSizeInBytes());

            super.addEventsPropertiesToJson(jsonWorkflowElement, ingestionData);
        }
        catch (Exception e)
        {
            mLogger.error("buildWorkflowJson failed: " + e);

            throw e;
        }

        return jsonWorkflowElement;
    }

    public void handleFileUpload(FileUploadEvent event)
    {
        mLogger.info("Uploaded binary file name: " + event.getFile().getFileName());

        // save
        File binaryFile = null;
        {
            InputStream input = null;
            OutputStream output = null;

            try
            {
                pushBinaryFileName = event.getFile().getFileName();
                binaryFile = new File(getLocalBinaryPathName(pushBinaryFileName));

                mLogger.info("copy from " + event.getFile() + " to " + binaryFile.getAbsolutePath());
                input = event.getFile().getInputstream();
                output = new FileOutputStream(binaryFile);

                IOUtils.copy(input, output);

                String fileName;
                String fileExtension = "";
                int extensionIndex = event.getFile().getFileName().lastIndexOf('.');
                if (extensionIndex == -1)
                    fileName = event.getFile().getFileName();
                else
                {
                    fileName = event.getFile().getFileName().substring(0, extensionIndex);
                    fileExtension = event.getFile().getFileName().substring(extensionIndex + 1);
                }

                if (!super.isLabelChanged())
                    super.setLabel(fileName);

                fileFormat = fileExtension;

                if (getTitle() == null || getTitle().isEmpty())   // not set yet
                    setTitle(fileName);
            }
            catch (Exception e)
            {
                mLogger.error("Exception: " + e.getMessage());

                return;
            }
            finally
            {
                if (input != null)
                    IOUtils.closeQuietly(input);
                if (output != null)
                    IOUtils.closeQuietly(output);
            }
        }
    }

    private String getLocalBinaryPathName(String fileName)
    {
        try
        {
            Long userKey = SessionUtils.getUserProfile().getUserKey();

            String localBinaryPathName = temporaryPushBinariesPathName + "/" + userKey + "-" + fileName;

            return localBinaryPathName;
        }
        catch (Exception e)
        {
            String errorMessage = "Exception: " + e;
            mLogger.error(errorMessage);

            return null;
        }
    }

    public String getSourceDownloadType() {
        return sourceDownloadType;
    }

    public void setSourceDownloadType(String sourceDownloadType) {
        this.sourceDownloadType = sourceDownloadType;
    }

    public String getPullSourceURL() {
        return pullSourceURL;
    }

    public void setPullSourceURL(String pullSourceURL) {
        this.pullSourceURL = pullSourceURL;
    }

    public String getPushBinaryFileName() {
        return pushBinaryFileName;
    }

    public void setPushBinaryFileName(String pushBinaryFileName) {
        this.pushBinaryFileName = pushBinaryFileName;
    }

    public String getFileFormat() {
        return fileFormat;
    }

    public void setFileFormat(String fileFormat) {
        this.fileFormat = fileFormat;
    }

    public String getMd5FileChecksum() {
        return md5FileChecksum;
    }

    public void setMd5FileChecksum(String md5FileChecksum) {
        this.md5FileChecksum = md5FileChecksum;
    }

    public Long getFileSizeInBytes() {
        return fileSizeInBytes;
    }

    public void setFileSizeInBytes(Long fileSizeInBytes) {
        this.fileSizeInBytes = fileSizeInBytes;
    }

    public String getVideoAudioAllowTypes() {
        return videoAudioAllowTypes;
    }

    public void setVideoAudioAllowTypes(String videoAudioAllowTypes) {
        this.videoAudioAllowTypes = videoAudioAllowTypes;
    }

    public String getTemporaryPushBinariesPathName() {
        return temporaryPushBinariesPathName;
    }

    public void setTemporaryPushBinariesPathName(String temporaryPushBinariesPathName) {
        this.temporaryPushBinariesPathName = temporaryPushBinariesPathName;
    }
}
